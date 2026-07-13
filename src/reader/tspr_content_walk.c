// Shared content-stream walker. One tokenizer + graphics-state machine that
// both the text extractor (tspr_text.c) and the lossy image measurer
// (tspr_lossy.c) drive through callbacks. It owns the parts the two clients
// had duplicated and drifted on:
//
//   - operand tokenization with resync (malformed operand -> +1 byte, drop
//     stack), overlong-operator handling, and true/false/null-as-operands;
//   - the q/Q graphics-state stack: the CTM plus a fixed-size, client-opaque
//     gstate blob (text keeps its text matrices/font there; lossy keeps
//     nothing). The stack starts on the C stack and grows on the heap up to a
//     cap; deeper nesting or an allocation failure sets `ctm_lost`, which the
//     lossy client reads to poison unmeasurable image placements — the text
//     client simply keeps drawing (its old fixed 32-deep stack silently
//     dropped saves past the cap, and dropping a save is strictly milder than
//     poisoning, so extraction is unchanged: see the report);
//   - cm composition onto the CTM;
//   - Form XObject recursion with /Matrix composition, a depth budget, and a
//     pointer-identity cycle guard, plus the resources rule both clients
//     already shared (use the form's own /Resources, else inherit the
//     caller's);
//   - BI ... ID <binary> EI inline-image skipping so raw image bytes never
//     derail the tokenizer.
//
// The client gets: an operator callback for everything it cares about (text
// operators for text; image Do with the effective CTM for lossy), plus
// optional save/restore hooks so a client whose per-gstate payload is larger
// than the blob (none today) could react. The walker never allocates the
// client's blob; it memcpy's a caller-sized region.

#include "tspr_internal.h"
#include "tspr_content_walk.h"
#include <stdlib.h>
#include <string.h>

// --- Matrix (PDF row-vector convention: [a b 0; c d 0; e f 1]) ---

const TspdfWalkMat TSPDF_WALK_IDENTITY = {1, 0, 0, 1, 0, 0};

TspdfWalkMat tspdf_walk_mat_mul(TspdfWalkMat m, TspdfWalkMat n) {
    TspdfWalkMat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

// --- Character classes (content-stream lexing) ---

static bool walk_is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' ||
           c == '\f';
}

static bool walk_is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static double walk_obj_num(const TspdfObj *o) {
    if (!o) return 0;
    if (o->type == TSPDF_OBJ_INT) return (double)o->integer;
    if (o->type == TSPDF_OBJ_REAL) return o->real;
    return 0;
}

static TspdfObj *walk_resolve(TspdfWalker *w, TspdfObj *o) {
    if (!o || o->type != TSPDF_OBJ_REF) return o;
    if (o->ref.num >= w->doc->xref.count) return NULL;
    return tspdf_xref_resolve(&w->doc->xref, w->rp, o->ref.num,
                              w->doc->obj_cache, w->doc->crypt);
}

TspdfObj *tspdf_walk_resolve(TspdfWalker *w, TspdfObj *o) {
    return walk_resolve(w, o);
}

// --- Inline image skip: BI ... ID <binary> EI ---

static void walk_skip_inline_image(TspdfParser *p) {
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'I' && p->data[p->pos + 1] == 'D' &&
            (p->pos + 2 >= p->len || walk_is_ws(p->data[p->pos + 2]))) {
            p->pos += 2;
            break;
        }
        p->pos++;
    }
    if (p->pos < p->len && walk_is_ws(p->data[p->pos])) p->pos++;
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'E' && p->data[p->pos + 1] == 'I' &&
            p->pos > 0 && walk_is_ws(p->data[p->pos - 1]) &&
            (p->pos + 2 >= p->len || walk_is_ws(p->data[p->pos + 2]) ||
             walk_is_delim(p->data[p->pos + 2]))) {
            p->pos += 2;
            return;
        }
        p->pos++;
    }
    p->pos = p->len;
}

// --- Form XObject recursion (Do dispatch to a Form) ---
//
// Returns true when `xo` was a Form and was entered (or refused because of a
// depth/cycle/load limit — either way the caller must not treat it as an
// image). Returns false when `xo` is not a Form (an image or unknown subtype),
// leaving that to the client's Do callback.

static bool walk_enter_form(TspdfWalker *w, TspdfObj *xo, uint32_t num,
                            TspdfWalkMat ctm, TspdfObj *resources, int depth,
                            bool ctm_lost, void *gstate) {
    TspdfObj *sub =
        walk_resolve(w, tspdf_dict_get(xo->stream.dict, "Subtype"));
    if (!sub || sub->type != TSPDF_OBJ_NAME ||
        strcmp((const char *)sub->string.data, "Form") != 0)
        return false;

    // form_stack is sized TSPDF_WALK_MAX_DEPTH; a larger max_depth would
    // index past it, so the array bound caps the recursion regardless.
    int cap = w->max_depth < TSPDF_WALK_MAX_DEPTH ? w->max_depth
                                                  : TSPDF_WALK_MAX_DEPTH;
    if (depth + 1 >= cap) return true;
    for (int i = 0; i <= depth; i++)
        if (w->form_stack[i] == xo) return true;  // cycle
    w->form_stack[depth + 1] = xo;

    size_t blen = 0;
    uint8_t *bytes = w->load_stream(w->client, xo, num, &blen);
    if (bytes) {
        TspdfWalkMat sub_ctm = ctm;
        TspdfObj *mtx =
            walk_resolve(w, tspdf_dict_get(xo->stream.dict, "Matrix"));
        if (mtx && mtx->type == TSPDF_OBJ_ARRAY && mtx->array.count >= 6) {
            TspdfWalkMat m;
            m.a = walk_obj_num(walk_resolve(w, &mtx->array.items[0]));
            m.b = walk_obj_num(walk_resolve(w, &mtx->array.items[1]));
            m.c = walk_obj_num(walk_resolve(w, &mtx->array.items[2]));
            m.d = walk_obj_num(walk_resolve(w, &mtx->array.items[3]));
            m.e = walk_obj_num(walk_resolve(w, &mtx->array.items[4]));
            m.f = walk_obj_num(walk_resolve(w, &mtx->array.items[5]));
            sub_ctm = tspdf_walk_mat_mul(m, ctm);
        }
        TspdfObj *sub_res =
            walk_resolve(w, tspdf_dict_get(xo->stream.dict, "Resources"));
        if (!sub_res || sub_res->type != TSPDF_OBJ_DICT) sub_res = resources;

        // The client's per-form gstate carry: text resets its text matrices on
        // form entry. It gets a private copy of the blob for the sub-stream.
        void *sub_gstate = gstate;
        unsigned char *sub_blob = NULL;
        if (w->gstate_size) {
            sub_blob = (unsigned char *)malloc(w->gstate_size);
            if (!sub_blob) {
                free(bytes);
                w->form_stack[depth + 1] = NULL;
                return true;
            }
            memcpy(sub_blob, gstate, w->gstate_size);
            sub_gstate = sub_blob;
            if (w->form_enter) w->form_enter(w->client, sub_gstate, xo);
        }

        tspdf_walk_run(w, bytes, blen, sub_ctm, sub_res, depth + 1, ctm_lost,
                       sub_gstate);
        free(sub_blob);
        w->load_stream_free(w->client, bytes);
    }
    w->form_stack[depth + 1] = NULL;
    return true;
}

// --- Core interpreter ---

void tspdf_walk_run(TspdfWalker *w, const uint8_t *data, size_t len,
                    TspdfWalkMat ctm, TspdfObj *resources, int depth,
                    bool ctm_lost, void *gstate) {
    TspdfParser p;
    tspdf_parser_init(&p, data, len, w->scratch);
    TspdfObj *stk[TSPDF_WALK_OP_STACK];
    int ns = 0;

    // q/Q save stack: CTM + a copy of the client gstate blob. Starts on the C
    // stack, grows on the heap to a cap; past it (or on OOM) ctm_lost latches
    // for the rest of this stream.
    typedef struct {
        TspdfWalkMat ctm;
    } SaveCtm;
    SaveCtm ctm_fixed[TSPDF_WALK_GS_STACK];
    SaveCtm *ctm_stack = ctm_fixed;
    SaveCtm *ctm_heap = NULL;
    // Parallel client-blob save stack (only allocated if the client uses one).
    unsigned char *blob_fixed = NULL;
    unsigned char *blob_stack = NULL;
    unsigned char *blob_heap = NULL;
    if (w->gstate_size) {
        blob_fixed =
            (unsigned char *)malloc((size_t)TSPDF_WALK_GS_STACK * w->gstate_size);
        blob_stack = blob_fixed;
        if (!blob_fixed) ctm_lost = true;  // cannot track saves: poison
    }
    size_t gs_cap = TSPDF_WALK_GS_STACK;
    size_t ngs = 0;

    while (1) {
        tspdf_skip_whitespace(&p);
        if (p.pos >= p.len) break;
        uint8_t c = p.data[p.pos];

        if (c == '(' || c == '<' || c == '[' || c == '/' || c == '+' ||
            c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            p.error = TSPDF_OK;
            size_t before = p.pos;
            TspdfObj *o = tspdf_parse_obj(&p);
            if (!o) {
                p.error = TSPDF_OK;
                p.pos = before + 1;
                ns = 0;
                continue;
            }
            if (ns == TSPDF_WALK_OP_STACK) {
                memmove(stk, stk + 1, sizeof(stk[0]) * (TSPDF_WALK_OP_STACK - 1));
                ns--;
            }
            stk[ns++] = o;
            continue;
        }
        if (walk_is_delim(c)) {  // stray delimiter: resync
            p.pos++;
            continue;
        }

        char op[8];
        size_t ol = 0;
        bool overlong = false;
        while (p.pos < p.len && !walk_is_ws(p.data[p.pos]) &&
               !walk_is_delim(p.data[p.pos]) && p.data[p.pos] > ' ') {
            if (ol + 1 < sizeof(op)) op[ol++] = (char)p.data[p.pos];
            else overlong = true;
            p.pos++;
        }
        op[ol] = '\0';
        if (ol == 0) {
            p.pos++;
            continue;
        }
        if (overlong) {
            ns = 0;
            continue;
        }
        if (strcmp(op, "true") == 0 || strcmp(op, "false") == 0 ||
            strcmp(op, "null") == 0) {
            continue;  // operands, not operators; keep the stack
        }

        if (strcmp(op, "q") == 0) {
            if (!ctm_lost && ngs == gs_cap) {
                size_t ncap = gs_cap * 2;
                SaveCtm *ncs = NULL;
                unsigned char *nbs = NULL;
                bool ok = ncap <= TSPDF_WALK_GS_MAX;
                if (ok) {
                    ncs = (SaveCtm *)realloc(ctm_heap, ncap * sizeof(SaveCtm));
                    if (!ncs) ok = false;
                }
                if (ok && w->gstate_size) {
                    nbs = (unsigned char *)realloc(blob_heap,
                                                   ncap * w->gstate_size);
                    if (!nbs) ok = false;
                    else blob_heap = nbs;
                }
                if (ok) {
                    if (!ctm_heap)
                        memcpy(ncs, ctm_fixed, sizeof(ctm_fixed));
                    ctm_heap = ncs;
                    ctm_stack = ncs;
                    if (w->gstate_size) {
                        if (blob_stack == blob_fixed)
                            memcpy(nbs, blob_fixed,
                                   (size_t)TSPDF_WALK_GS_STACK * w->gstate_size);
                        blob_stack = nbs;
                    }
                    gs_cap = ncap;
                } else {
                    if (ncs) ctm_heap = ctm_stack = ncs;  // keep grown CTM buf
                    ctm_lost = true;  // too deep / OOM: stop trusting the CTM
                }
            }
            if (!ctm_lost) {
                ctm_stack[ngs].ctm = ctm;
                if (w->gstate_size)
                    memcpy(blob_stack + ngs * w->gstate_size, gstate,
                           w->gstate_size);
                ngs++;
            }
        } else if (strcmp(op, "Q") == 0) {
            if (!ctm_lost && ngs > 0) {
                ngs--;
                ctm = ctm_stack[ngs].ctm;
                if (w->gstate_size)
                    memcpy(gstate, blob_stack + ngs * w->gstate_size,
                           w->gstate_size);
            }
        } else if (strcmp(op, "cm") == 0) {
            TspdfWalkMat m;
            m.a = walk_obj_num(ns >= 6 ? stk[ns - 6] : NULL);
            m.b = walk_obj_num(ns >= 5 ? stk[ns - 5] : NULL);
            m.c = walk_obj_num(ns >= 4 ? stk[ns - 4] : NULL);
            m.d = walk_obj_num(ns >= 3 ? stk[ns - 3] : NULL);
            m.e = walk_obj_num(ns >= 2 ? stk[ns - 2] : NULL);
            m.f = walk_obj_num(ns >= 1 ? stk[ns - 1] : NULL);
            ctm = tspdf_walk_mat_mul(m, ctm);
        } else if (strcmp(op, "Do") == 0) {
            TspdfObj *name = ns >= 1 ? stk[ns - 1] : NULL;
            if (name && name->type == TSPDF_OBJ_NAME && resources) {
                TspdfObj *xobjs =
                    walk_resolve(w, tspdf_dict_get(resources, "XObject"));
                TspdfObj *ref =
                    xobjs && xobjs->type == TSPDF_OBJ_DICT
                        ? tspdf_dict_get(xobjs, (const char *)name->string.data)
                        : NULL;
                TspdfObj *xo = walk_resolve(w, ref);
                if (xo && xo->type == TSPDF_OBJ_STREAM) {
                    uint32_t num =
                        ref && ref->type == TSPDF_OBJ_REF ? ref->ref.num : 0;
                    // Try Form recursion; if not a Form, hand the image (or
                    // unknown subtype) to the client with the effective CTM.
                    if (!walk_enter_form(w, xo, num, ctm, resources, depth,
                                         ctm_lost, gstate)) {
                        if (w->image_do)
                            w->image_do(w->client, xo, num, ctm, ctm_lost);
                    }
                }
            }
        } else if (strcmp(op, "BI") == 0) {
            walk_skip_inline_image(&p);
        } else if (w->op) {
            // Everything else goes to the client (text operators; lossy
            // ignores). The client reads operands via the shared helpers.
            w->op(w->client, op, stk, ns, ctm, resources, gstate);
        }
        ns = 0;
    }
    free(ctm_heap);
    free(blob_heap);
    free(blob_fixed);
}

// --- Operand accessors for clients ---

TspdfObj *tspdf_walk_operand(TspdfObj **stk, int ns, int from_top) {
    int i = ns - 1 - from_top;
    return i >= 0 ? stk[i] : NULL;
}

double tspdf_walk_operand_num(TspdfObj **stk, int ns, int from_top) {
    return walk_obj_num(tspdf_walk_operand(stk, ns, from_top));
}
