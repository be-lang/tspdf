// Lossy raster-image recompression for `tspdf compress --lossy`: find every
// drawn image XObject, work out how large it actually renders (walking the
// content streams with full CTM tracking, including Form XObjects), then
// downsample oversized 8-bit RGB/Gray images with a box filter and re-encode
// them as baseline JPEG (src/image/jpeg_codec.c). Conservative by design:
// anything unusual (Indexed/ICC color, masks, multi-filter chains,
// progressive JPEG input, non-default /Decode) passes through untouched, and
// the original stream is kept unless the new one is at least 10% smaller.
//
// DPI analysis notes:
//  - An image XObject paints the unit square under the CTM, so its rendered
//    size in points is |column vectors| of the CTM: w = hypot(a,b),
//    h = hypot(c,d). Rotation/skew are therefore handled.
//  - Form XObjects are entered recursively (depth-capped, cycle-guarded)
//    with the form /Matrix composed onto the caller's CTM and the form's own
//    /Resources (falling back to the caller's when absent), so images drawn
//    only inside forms get a real measured size rather than a guess.
//  - Multiple placements: the largest rendered extent per axis wins (the
//    placement needing the most pixels). Images never drawn are skipped.
//  - /UserUnit is ignored (rare; would only make us keep more pixels).
//  - Only page /Contents (and Form XObjects reached from them) are scanned.
//    Annotation appearance streams, tiling patterns, and Type3 CharProcs are
//    not, so images drawn only there count as never drawn and are left
//    untouched; but an image drawn both on a page and larger inside an
//    annotation appearance could be downsampled to the page extent
//    (accepted limitation).

#include "tspr_internal.h"
#include "../image/jpeg_codec.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- Pure pixel helpers (unit-tested directly) ---

bool tspdf_lossy_box_downsample(const uint8_t *src, uint32_t sw, uint32_t sh,
                                int comps, uint8_t *dst, uint32_t dw, uint32_t dh) {
    if (!src || !dst || sw == 0 || sh == 0 || dw == 0 || dh == 0 || comps <= 0)
        return false;
    if (dw > sw) dw = sw;  // this filter never upsamples
    if (dh > sh) dh = sh;

    const double xs = (double)sw / dw;  // source pixels per dest bin, >= 1
    const double ys = (double)sh / dh;
    const size_t drow = (size_t)dw * (size_t)comps;

    // One horizontally-reduced source row, plus two dest-row accumulators
    // (a source row overlaps at most two dest rows since ys >= 1).
    double *hacc = (double *)malloc(drow * sizeof(double));
    double *acc = (double *)calloc(drow * 2, sizeof(double));
    if (!hacc || !acc) {
        free(hacc);
        free(acc);
        return false;
    }

    const double inv_area = 1.0 / (xs * ys);
    uint32_t next_out = 0;  // next dest row to emit

    for (uint32_t sy = 0; sy < sh; sy++) {
        // Horizontal reduction with exact edge weights: source pixel sx
        // (covering [sx, sx+1)) splits between dest bin j = floor(sx/xs)
        // and bin j+1 where the bin boundary (j+1)*xs falls inside it.
        memset(hacc, 0, drow * sizeof(double));
        const uint8_t *sp = src + (size_t)sy * sw * (size_t)comps;
        for (uint32_t sx = 0; sx < sw; sx++) {
            uint32_t j = (uint32_t)((double)sx / xs);
            if (j >= dw) j = dw - 1;
            double w0 = (j + 1) * xs - (double)sx;  // overlap with bin j
            if (w0 > 1.0) w0 = 1.0;
            if (w0 < 0.0) w0 = 0.0;
            const double w1 = 1.0 - w0;
            const uint8_t *px = sp + (size_t)sx * (size_t)comps;
            double *b0 = hacc + (size_t)j * (size_t)comps;
            for (int k = 0; k < comps; k++) b0[k] += w0 * px[k];
            if (w1 > 1e-12 && j + 1 < dw) {
                double *b1 = b0 + comps;
                for (int k = 0; k < comps; k++) b1[k] += w1 * px[k];
            }
        }

        // Vertical distribution of this reduced row (same split, per row).
        uint32_t r = (uint32_t)((double)sy / ys);
        if (r >= dh) r = dh - 1;
        double v0 = (r + 1) * ys - (double)sy;
        if (v0 > 1.0) v0 = 1.0;
        if (v0 < 0.0) v0 = 0.0;
        const double v1 = 1.0 - v0;
        double *a0 = acc + (size_t)(r & 1) * drow;
        for (size_t k = 0; k < drow; k++) a0[k] += v0 * hacc[k];
        if (v1 > 1e-12 && r + 1 < dh) {
            double *a1 = acc + (size_t)((r + 1) & 1) * drow;
            for (size_t k = 0; k < drow; k++) a1[k] += v1 * hacc[k];
        }

        // Emit every dest row whose source span [row*ys, (row+1)*ys) is now
        // fully consumed.
        while (next_out < dh &&
               ((double)sy + 1.0) >= (double)(next_out + 1) * ys - 1e-9) {
            double *a = acc + (size_t)(next_out & 1) * drow;
            uint8_t *dp = dst + (size_t)next_out * drow;
            for (size_t k = 0; k < drow; k++) {
                double v = a[k] * inv_area + 0.5;
                dp[k] = v <= 0.0 ? 0 : v >= 255.0 ? 255 : (uint8_t)v;
            }
            memset(a, 0, drow * sizeof(double));
            next_out++;
        }
    }
    // Floating-point slack can leave the last row unemitted.
    while (next_out < dh) {
        double *a = acc + (size_t)(next_out & 1) * drow;
        uint8_t *dp = dst + (size_t)next_out * drow;
        for (size_t k = 0; k < drow; k++) {
            double v = a[k] * inv_area + 0.5;
            dp[k] = v <= 0.0 ? 0 : v >= 255.0 ? 255 : (uint8_t)v;
        }
        memset(a, 0, drow * sizeof(double));
        next_out++;
    }

    free(hacc);
    free(acc);
    return true;
}

bool tspdf_lossy_rgb_is_gray(const uint8_t *rgb, size_t npix) {
    if (!rgb) return false;
    for (size_t i = 0; i < npix; i++) {
        const uint8_t *p = rgb + i * 3;
        int r = p[0], g = p[1], b = p[2];
        int d0 = r > g ? r - g : g - r;
        int d1 = r > b ? r - b : b - r;
        int d2 = g > b ? g - b : b - g;
        if (d0 > 8 || d1 > 8 || d2 > 8) return false;
    }
    return true;
}

void tspdf_lossy_rgb_to_gray(const uint8_t *rgb, size_t npix, uint8_t *out) {
    if (!rgb || !out) return;
    for (size_t i = 0; i < npix; i++) {
        const uint8_t *p = rgb + i * 3;
        out[i] = (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
    }
}

bool tspdf_lossy_target_dims(uint32_t w, uint32_t h, double w_pt, double h_pt,
                             int target_dpi, uint32_t *dw, uint32_t *dh) {
    if (w == 0 || h == 0 || w_pt <= 0.0 || h_pt <= 0.0 || target_dpi <= 0 ||
        !dw || !dh)
        return false;

    double tw = (double)target_dpi * w_pt / 72.0;  // pixels needed at target
    double th = (double)target_dpi * h_pt / 72.0;
    if (tw < 1.0) tw = 1.0;
    if (th < 1.0) th = 1.0;

    const double s = fmax((double)w / tw, (double)h / th);
    if (s <= 1.3) return false;  // near target already: not worth re-encoding

    uint32_t nw = (uint32_t)((double)w / s + 0.5);
    uint32_t nh = (uint32_t)((double)h / s + 0.5);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > w) nw = w;  // never upsample
    if (nh > h) nh = h;
    *dw = nw;
    *dh = nh;
    return true;
}

bool tspdf_lossy_worth_replacing(size_t old_stored, size_t new_stored) {
    // Replace only when the new stream is at least 10% smaller: exact
    // integer form of new <= 0.9 * old (stream sizes cannot overflow this).
    if (old_stored == 0) return false;
    return new_stored * 10 <= old_stored * 9;
}

// --- Content-stream walker (CTM tracking to each image Do) ---

typedef struct {
    double a, b, c, d, e, f;
} LossyMat;

static const LossyMat LOSSY_IDENTITY = {1, 0, 0, 1, 0, 0};

static LossyMat lossy_mat_mul(LossyMat m, LossyMat n) {
    LossyMat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

// Per-object usage accumulator, indexed by object number.
typedef struct {
    double w_pt, h_pt;
    bool drawn;
    bool lost;  // drawn while the CTM was untracked: never downsample
} LossyUse;

#define LOSSY_MAX_DEPTH 8       // Form XObject recursion cap
#define LOSSY_OP_STACK 8        // operands kept for cm/Do
#define LOSSY_GS_STACK 64       // initial q/Q save-stack capacity (heap-grown)
#define LOSSY_GS_MAX 4096       // growth cap; deeper nesting loses CTM tracking
#define LOSSY_CONTENT_MAX (64u << 20)

typedef struct {
    TspdfReader *doc;
    TspdfParser rp;             // object resolution (doc arena)
    TspdfArena scratch;         // transient operand parsing
    LossyUse *use;              // indexed by obj num, doc->xref.count entries
    const TspdfObj *form_stack[LOSSY_MAX_DEPTH];
} LossyCtx;

static TspdfObj *lossy_resolve(LossyCtx *ctx, TspdfObj *o) {
    if (!o || o->type != TSPDF_OBJ_REF) return o;
    if (o->ref.num >= ctx->doc->xref.count) return NULL;
    return tspdf_xref_resolve(&ctx->doc->xref, &ctx->rp, o->ref.num,
                              ctx->doc->obj_cache, ctx->doc->crypt);
}

static double lossy_obj_num(const TspdfObj *o) {
    if (!o) return 0;
    if (o->type == TSPDF_OBJ_INT) return (double)o->integer;
    if (o->type == TSPDF_OBJ_REAL) return o->real;
    return 0;
}

// Raw (stored, decrypted-but-not-decoded) stream bytes. *owned is a malloc'd
// buffer to free (NULL when the bytes point into the source or arena).
static const uint8_t *lossy_raw_stream(TspdfReader *doc, TspdfObj *s,
                                       uint32_t obj_num, size_t *out_len,
                                       uint8_t **owned) {
    *owned = NULL;
    if (!s || s->type != TSPDF_OBJ_STREAM) return NULL;
    if (s->stream.self_contained && s->stream.data) {
        *out_len = s->stream.len;
        return s->stream.data;
    }
    if (!doc->data || s->stream.raw_offset > doc->data_len ||
        s->stream.raw_len > doc->data_len - s->stream.raw_offset)
        return NULL;
    const uint8_t *raw = doc->data + s->stream.raw_offset;
    size_t raw_len = s->stream.raw_len;
    if (doc->crypt && obj_num > 0 && raw_len > 0) {
        size_t dec_len = 0;
        uint16_t gen = obj_num < doc->xref.count ? doc->xref.entries[obj_num].gen : 0;
        uint8_t *dec = tspdf_crypt_decrypt_stream(doc->crypt, obj_num, gen,
                                                  raw, raw_len, &dec_len);
        if (!dec) return NULL;
        *owned = dec;
        *out_len = dec_len;
        return dec;
    }
    *out_len = raw_len;
    return raw;
}

// Decrypt + run the filter chain; malloc'd result.
static uint8_t *lossy_load_stream(LossyCtx *ctx, TspdfObj *s, uint32_t obj_num,
                                  size_t *out_len) {
    uint8_t *owned = NULL;
    size_t raw_len = 0;
    const uint8_t *raw = lossy_raw_stream(ctx->doc, s, obj_num, &raw_len, &owned);
    if (!raw) return NULL;
    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    TspdfError err = tspdf_stream_decode(s->stream.dict, raw, raw_len,
                                         &decoded, &decoded_len);
    free(owned);
    if (err != TSPDF_OK) return NULL;
    *out_len = decoded_len;
    return decoded;
}

static bool lossy_is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}

static bool lossy_is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

// Skip BI ... ID <binary> EI so raw inline-image bytes cannot derail the
// tokenizer (same approach as the text extractor).
static void lossy_skip_inline_image(TspdfParser *p) {
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'I' && p->data[p->pos + 1] == 'D' &&
            (p->pos + 2 >= p->len || lossy_is_ws(p->data[p->pos + 2]))) {
            p->pos += 2;
            break;
        }
        p->pos++;
    }
    if (p->pos < p->len && lossy_is_ws(p->data[p->pos])) p->pos++;
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'E' && p->data[p->pos + 1] == 'I' &&
            p->pos > 0 && lossy_is_ws(p->data[p->pos - 1]) &&
            (p->pos + 2 >= p->len || lossy_is_ws(p->data[p->pos + 2]) ||
             lossy_is_delim(p->data[p->pos + 2]))) {
            p->pos += 2;
            return;
        }
        p->pos++;
    }
    p->pos = p->len;
}

static void lossy_interpret(LossyCtx *ctx, const uint8_t *data, size_t len,
                            LossyMat ctm, TspdfObj *resources, int depth,
                            bool ctm_lost);

static void lossy_do_xobject(LossyCtx *ctx, LossyMat ctm, TspdfObj *resources,
                             const TspdfObj *name, int depth, bool ctm_lost) {
    if (!resources || !name || name->type != TSPDF_OBJ_NAME) return;
    TspdfObj *xobjs = lossy_resolve(ctx, tspdf_dict_get(resources, "XObject"));
    if (!xobjs || xobjs->type != TSPDF_OBJ_DICT) return;
    TspdfObj *ref = tspdf_dict_get(xobjs, (const char *)name->string.data);
    uint32_t num = ref && ref->type == TSPDF_OBJ_REF ? ref->ref.num : 0;
    TspdfObj *xo = lossy_resolve(ctx, ref);
    if (!xo || xo->type != TSPDF_OBJ_STREAM) return;
    TspdfObj *sub = lossy_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Subtype"));
    if (!sub || sub->type != TSPDF_OBJ_NAME) return;

    if (strcmp((const char *)sub->string.data, "Image") == 0) {
        if (num == 0 || num >= ctx->doc->xref.count) return;
        LossyUse *u = &ctx->use[num];
        if (ctm_lost) {
            // The CTM is untrustworthy (q nesting outgrew the save stack):
            // this placement is unmeasurable, so the image must never be
            // downsampled, no matter what other placements measured.
            u->lost = true;
            return;
        }
        // The image fills the unit square under the CTM; the rendered size
        // in points is the length of the transformed basis vectors.
        double w = hypot(ctm.a, ctm.b);
        double h = hypot(ctm.c, ctm.d);
        if (w > u->w_pt) u->w_pt = w;
        if (h > u->h_pt) u->h_pt = h;
        u->drawn = true;
        return;
    }

    if (strcmp((const char *)sub->string.data, "Form") != 0) return;
    if (depth + 1 >= LOSSY_MAX_DEPTH) return;
    for (int i = 0; i <= depth; i++)
        if (ctx->form_stack[i] == xo) return;  // cycle
    ctx->form_stack[depth + 1] = xo;

    size_t blen = 0;
    uint8_t *bytes = lossy_load_stream(ctx, xo, num, &blen);
    if (bytes) {
        LossyMat sub_ctm = ctm;
        TspdfObj *mtx = lossy_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Matrix"));
        if (mtx && mtx->type == TSPDF_OBJ_ARRAY && mtx->array.count >= 6) {
            LossyMat m;
            m.a = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[0]));
            m.b = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[1]));
            m.c = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[2]));
            m.d = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[3]));
            m.e = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[4]));
            m.f = lossy_obj_num(lossy_resolve(ctx, &mtx->array.items[5]));
            sub_ctm = lossy_mat_mul(m, ctm);
        }
        TspdfObj *sub_res = lossy_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Resources"));
        if (!sub_res || sub_res->type != TSPDF_OBJ_DICT) sub_res = resources;
        lossy_interpret(ctx, bytes, blen, sub_ctm, sub_res, depth + 1, ctm_lost);
        free(bytes);
    }
    ctx->form_stack[depth + 1] = NULL;
}

static void lossy_interpret(LossyCtx *ctx, const uint8_t *data, size_t len,
                            LossyMat ctm, TspdfObj *resources, int depth,
                            bool ctm_lost) {
    TspdfParser p;
    tspdf_parser_init(&p, data, len, &ctx->scratch);
    TspdfObj *stk[LOSSY_OP_STACK];
    int ns = 0;
    // q/Q save stack: starts on the C stack, grows on the heap up to
    // LOSSY_GS_MAX. If nesting goes deeper still (or the realloc fails),
    // ctm_lost is set for the rest of this content stream: an unmatched Q
    // would otherwise restore an ancestor CTM too early and make a large
    // image look tiny, so from then on every image Do is marked
    // unmeasurable and skipped instead.
    LossyMat gs_fixed[LOSSY_GS_STACK];
    LossyMat *gstack = gs_fixed;
    LossyMat *gs_heap = NULL;
    size_t gs_cap = LOSSY_GS_STACK;
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
            if (ns == LOSSY_OP_STACK) {
                memmove(stk, stk + 1, sizeof(stk[0]) * (LOSSY_OP_STACK - 1));
                ns--;
            }
            stk[ns++] = o;
            continue;
        }
        if (lossy_is_delim(c)) {  // stray delimiter: resync
            p.pos++;
            continue;
        }

        char op[8];
        size_t ol = 0;
        bool overlong = false;
        while (p.pos < p.len && !lossy_is_ws(p.data[p.pos]) &&
               !lossy_is_delim(p.data[p.pos]) && p.data[p.pos] > ' ') {
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
            continue;  // operands, not operators
        }

        if (strcmp(op, "q") == 0) {
            if (!ctm_lost && ngs == gs_cap) {
                LossyMat *nh = NULL;
                size_t ncap = gs_cap * 2;
                if (ncap <= LOSSY_GS_MAX)
                    nh = (LossyMat *)realloc(gs_heap, ncap * sizeof(LossyMat));
                if (nh) {
                    if (!gs_heap) memcpy(nh, gs_fixed, sizeof(gs_fixed));
                    gs_heap = nh;
                    gstack = nh;
                    gs_cap = ncap;
                } else {
                    ctm_lost = true;  // too deep / OOM: stop trusting the CTM
                }
            }
            if (!ctm_lost) gstack[ngs++] = ctm;
        } else if (strcmp(op, "Q") == 0) {
            if (!ctm_lost && ngs > 0) ctm = gstack[--ngs];
        } else if (strcmp(op, "cm") == 0 && ns >= 6) {
            LossyMat m;
            m.a = lossy_obj_num(stk[ns - 6]);
            m.b = lossy_obj_num(stk[ns - 5]);
            m.c = lossy_obj_num(stk[ns - 4]);
            m.d = lossy_obj_num(stk[ns - 3]);
            m.e = lossy_obj_num(stk[ns - 2]);
            m.f = lossy_obj_num(stk[ns - 1]);
            ctm = lossy_mat_mul(m, ctm);
        } else if (strcmp(op, "Do") == 0 && ns >= 1) {
            lossy_do_xobject(ctx, ctm, resources, stk[ns - 1], depth, ctm_lost);
        } else if (strcmp(op, "BI") == 0) {
            lossy_skip_inline_image(&p);
        }
        // every other operator: irrelevant to image placement
        ns = 0;
    }
    free(gs_heap);
}

// Page /Resources with Pages-tree inheritance via the /Parent chain.
static TspdfObj *lossy_page_resources(LossyCtx *ctx, TspdfObj *page_dict) {
    TspdfObj *cur = page_dict;
    for (size_t depth = 0; cur && cur->type == TSPDF_OBJ_DICT && depth < 64; depth++) {
        TspdfObj *res = lossy_resolve(ctx, tspdf_dict_get(cur, "Resources"));
        if (res && res->type == TSPDF_OBJ_DICT) return res;
        cur = lossy_resolve(ctx, tspdf_dict_get(cur, "Parent"));
    }
    return NULL;
}

// Concatenate a page's /Contents streams ('\n' between).
static uint8_t *lossy_page_content(LossyCtx *ctx, TspdfObj *page_dict,
                                   size_t *out_len) {
    TspdfObj *contents = tspdf_dict_get(page_dict, "Contents");
    if (!contents) return NULL;

    TspdfObj *single = NULL;
    TspdfObj *arr = NULL;
    if (contents->type == TSPDF_OBJ_ARRAY) {
        arr = contents;
    } else {
        TspdfObj *resolved = lossy_resolve(ctx, contents);
        if (resolved && resolved->type == TSPDF_OBJ_ARRAY) arr = resolved;
        else single = contents;
    }

    size_t n = arr ? arr->array.count : 1;
    uint8_t *buf = NULL;
    size_t buf_len = 0, buf_cap = 0;
    for (size_t i = 0; i < n; i++) {
        TspdfObj *item = arr ? &arr->array.items[i] : single;
        uint32_t num = item && item->type == TSPDF_OBJ_REF ? item->ref.num : 0;
        TspdfObj *s = lossy_resolve(ctx, item);
        if (!s || s->type != TSPDF_OBJ_STREAM) continue;
        size_t slen = 0;
        uint8_t *bytes = lossy_load_stream(ctx, s, num, &slen);
        if (!bytes) continue;
        if (slen >= LOSSY_CONTENT_MAX - buf_len) {
            free(bytes);
            break;
        }
        if (buf_len + slen + 1 > buf_cap) {
            size_t want = (buf_len + slen + 1) * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, want);
            if (!nb) {
                free(bytes);
                break;
            }
            buf = nb;
            buf_cap = want;
        }
        memcpy(buf + buf_len, bytes, slen);
        buf_len += slen;
        buf[buf_len++] = '\n';
        free(bytes);
    }
    *out_len = buf_len;
    return buf;
}

TspdfError tspdf_lossy_scan_placements(TspdfReader *doc,
                                       TspdfLossyPlacement **out, size_t *count) {
    if (!doc || !out || !count) return TSPDF_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;
    if (doc->xref.count == 0) return TSPDF_OK;

    LossyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.doc = doc;
    tspdf_parser_init(&ctx.rp, doc->data, doc->data_len, &doc->arena);
    ctx.scratch = tspdf_arena_create(1 << 16);
    ctx.use = (LossyUse *)calloc(doc->xref.count, sizeof(LossyUse));
    if (!ctx.use) {
        tspdf_arena_destroy(&ctx.scratch);
        return TSPDF_ERR_ALLOC;
    }

    for (size_t i = 0; i < doc->pages.count; i++) {
        TspdfObj *page = doc->pages.pages[i].page_dict;
        if (!page || page->type != TSPDF_OBJ_DICT) continue;
        TspdfObj *res = lossy_page_resources(&ctx, page);
        size_t clen = 0;
        uint8_t *content = lossy_page_content(&ctx, page, &clen);
        if (content) {
            lossy_interpret(&ctx, content, clen, LOSSY_IDENTITY, res, 0, false);
            free(content);
        }
        tspdf_arena_reset(&ctx.scratch);
    }

    // An image with any unmeasurable placement (ctx.use[i].lost) is excluded
    // entirely: downsizing it to some other, smaller placement could destroy
    // the placement we could not measure.
    size_t n = 0;
    for (size_t i = 0; i < doc->xref.count; i++)
        if (ctx.use[i].drawn && !ctx.use[i].lost) n++;

    if (n > 0) {
        TspdfLossyPlacement *pl =
            (TspdfLossyPlacement *)malloc(n * sizeof(TspdfLossyPlacement));
        if (!pl) {
            free(ctx.use);
            tspdf_arena_destroy(&ctx.scratch);
            return TSPDF_ERR_ALLOC;
        }
        size_t j = 0;
        for (size_t i = 0; i < doc->xref.count; i++) {
            if (!ctx.use[i].drawn || ctx.use[i].lost) continue;
            pl[j].obj_num = (uint32_t)i;
            pl[j].w_pt = ctx.use[i].w_pt;
            pl[j].h_pt = ctx.use[i].h_pt;
            j++;
        }
        *out = pl;
        *count = n;
    }

    free(ctx.use);
    tspdf_arena_destroy(&ctx.scratch);
    return TSPDF_OK;
}

// --- Eligibility + rewrite ---

// Cap decoded pixel buffers (w*h*comps) at 512 MB so a hostile PDF cannot
// demand an absurd allocation. The acceptance-class 600-dpi page scan
// (~5000x7000 RGB, ~104 MB) fits comfortably.
#define LOSSY_MAX_PIXEL_BYTES ((size_t)512 << 20)

typedef enum {
    LOSSY_CS_NONE,
    LOSSY_CS_GRAY,
    LOSSY_CS_RGB,
} LossyColorSpace;

static LossyColorSpace lossy_colorspace(LossyCtx *ctx, TspdfObj *dict) {
    TspdfObj *cs = lossy_resolve(ctx, tspdf_dict_get(dict, "ColorSpace"));
    if (!cs || cs->type != TSPDF_OBJ_NAME) return LOSSY_CS_NONE;
    if (strcmp((const char *)cs->string.data, "DeviceRGB") == 0) return LOSSY_CS_RGB;
    if (strcmp((const char *)cs->string.data, "DeviceGray") == 0) return LOSSY_CS_GRAY;
    return LOSSY_CS_NONE;  // Indexed/ICCBased/CMYK/Lab/...: pass through
}

// The single stream filter name, accepting a one-element array; NULL when
// unfiltered or a multi-filter chain.
static const char *lossy_single_filter(LossyCtx *ctx, TspdfObj *dict) {
    TspdfObj *f = lossy_resolve(ctx, tspdf_dict_get(dict, "Filter"));
    if (!f) return NULL;
    if (f->type == TSPDF_OBJ_ARRAY) {
        if (f->array.count != 1) return NULL;
        f = lossy_resolve(ctx, &f->array.items[0]);
    }
    if (!f || f->type != TSPDF_OBJ_NAME) return NULL;
    return (const char *)f->string.data;
}

// True when /Decode is absent or exactly the default [0 1 0 1 ...].
static bool lossy_decode_is_default(LossyCtx *ctx, TspdfObj *dict, int comps) {
    TspdfObj *d = lossy_resolve(ctx, tspdf_dict_get(dict, "Decode"));
    if (!d) return true;
    if (d->type != TSPDF_OBJ_ARRAY || d->array.count != (size_t)(2 * comps))
        return false;
    for (size_t i = 0; i < d->array.count; i++) {
        double v = lossy_obj_num(lossy_resolve(ctx, &d->array.items[i]));
        double want = (i % 2 == 0) ? 0.0 : 1.0;
        if (v != want) return false;
    }
    return true;
}

static bool lossy_dict_int(LossyCtx *ctx, TspdfObj *dict, const char *key,
                           int64_t *out) {
    TspdfObj *o = lossy_resolve(ctx, tspdf_dict_get(dict, key));
    if (!o || o->type != TSPDF_OBJ_INT) return false;
    *out = o->integer;
    return true;
}

static bool lossy_dict_is_true(LossyCtx *ctx, TspdfObj *dict, const char *key) {
    TspdfObj *o = lossy_resolve(ctx, tspdf_dict_get(dict, key));
    return o && o->type == TSPDF_OBJ_BOOL && o->boolean;
}

static TspdfObj *lossy_make_name(TspdfArena *a, const char *name) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!o) return NULL;
    size_t n = strlen(name);
    uint8_t *s = (uint8_t *)tspdf_arena_alloc(a, n + 1);
    if (!s) return NULL;
    memcpy(s, name, n + 1);
    o->type = TSPDF_OBJ_NAME;
    o->string.data = s;
    o->string.len = n;
    return o;
}

static TspdfObj *lossy_make_int(TspdfArena *a, int64_t v) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!o) return NULL;
    o->type = TSPDF_OBJ_INT;
    o->integer = v;
    return o;
}

// Rebuild the image dict for the re-encoded JPEG: keep unrelated entries,
// drop filter/decode-parameter/size/colorspace entries and re-add them.
static bool lossy_rewrite_dict(TspdfReader *doc, TspdfObj *dict,
                               uint32_t dw, uint32_t dh, bool gray,
                               size_t jpeg_len) {
    TspdfArena *a = &doc->arena;
    size_t kept = 0;
    static const char *replaced[] = {
        "Filter", "DecodeParms", "DP", "Width", "Height",
        "ColorSpace", "Decode", "Length", NULL,
    };

    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc_zero(
        a, (dict->dict.count + 5) * sizeof(TspdfDictEntry));
    if (!entries) return false;

    for (size_t i = 0; i < dict->dict.count; i++) {
        const char *key = dict->dict.entries[i].key;
        bool skip = false;
        for (int r = 0; replaced[r]; r++) {
            if (strcmp(key, replaced[r]) == 0) {
                skip = true;
                break;
            }
        }
        if (skip) continue;
        entries[kept++] = dict->dict.entries[i];
    }

    struct {
        const char *key;
        TspdfObj *val;
    } add[5] = {
        {"Width", lossy_make_int(a, dw)},
        {"Height", lossy_make_int(a, dh)},
        {"ColorSpace", lossy_make_name(a, gray ? "DeviceGray" : "DeviceRGB")},
        {"Filter", lossy_make_name(a, "DCTDecode")},
        {"Length", lossy_make_int(a, (int64_t)jpeg_len)},
    };
    for (int i = 0; i < 5; i++) {
        if (!add[i].val) return false;
        size_t klen = strlen(add[i].key);
        char *k = (char *)tspdf_arena_alloc(a, klen + 1);
        if (!k) return false;
        memcpy(k, add[i].key, klen + 1);
        entries[kept].key = k;
        entries[kept].value = add[i].val;
        kept++;
    }

    dict->dict.entries = entries;
    dict->dict.count = kept;
    return true;
}

// Examine one drawn image XObject and re-encode it when eligible and
// worthwhile. Returns true when the stream was replaced.
static bool lossy_process_image(LossyCtx *ctx, uint32_t obj_num,
                                double w_pt, double h_pt, int target_dpi,
                                int quality, TspdfLossyStats *stats) {
    TspdfReader *doc = ctx->doc;
    TspdfObj *obj = tspdf_xref_resolve(&doc->xref, &ctx->rp, obj_num,
                                       doc->obj_cache, doc->crypt);
    if (!obj || obj->type != TSPDF_OBJ_STREAM) return false;
    TspdfObj *dict = obj->stream.dict;

    // Eligibility gates: 8-bit DeviceRGB/DeviceGray, no masks, default
    // decode, big enough to matter, single Flate or DCT filter.
    int64_t bpc = 0, w64 = 0, h64 = 0;
    if (!lossy_dict_int(ctx, dict, "BitsPerComponent", &bpc) || bpc != 8) return false;
    if (lossy_dict_is_true(ctx, dict, "ImageMask")) return false;
    if (tspdf_dict_get(dict, "SMask") || tspdf_dict_get(dict, "Mask")) return false;
    if (!lossy_dict_int(ctx, dict, "Width", &w64) ||
        !lossy_dict_int(ctx, dict, "Height", &h64))
        return false;
    if (w64 <= 0 || h64 <= 0 || w64 > INT32_MAX || h64 > INT32_MAX) return false;
    uint32_t w = (uint32_t)w64, h = (uint32_t)h64;
    if ((uint64_t)w * h < 65536) return false;  // too small to matter

    LossyColorSpace cs = lossy_colorspace(ctx, dict);
    if (cs == LOSSY_CS_NONE) return false;
    int comps = cs == LOSSY_CS_RGB ? 3 : 1;
    if ((uint64_t)w * h > LOSSY_MAX_PIXEL_BYTES / (unsigned)comps) return false;
    if (!lossy_decode_is_default(ctx, dict, comps)) return false;

    const char *filter = lossy_single_filter(ctx, dict);
    if (!filter) return false;
    bool is_flate = strcmp(filter, "FlateDecode") == 0;
    bool is_dct = strcmp(filter, "DCTDecode") == 0;
    if (!is_flate && !is_dct) return false;

    // Downsample decision from the measured placement.
    uint32_t dw = 0, dh = 0;
    if (!tspdf_lossy_target_dims(w, h, w_pt, h_pt, target_dpi, &dw, &dh))
        return false;

    // Fetch the stored bytes (the size baseline for the keep rule).
    uint8_t *owned = NULL;
    size_t stored_len = 0;
    const uint8_t *stored = lossy_raw_stream(doc, obj, obj_num, &stored_len, &owned);
    if (!stored || stored_len == 0) {
        free(owned);
        return false;
    }

    // Decode to raw pixels.
    TspdfArena work = tspdf_arena_create(1 << 20);
    uint8_t *pixels = NULL;         // w*h*comps interleaved
    uint8_t *flate_pixels = NULL;   // malloc'd (flate path)
    bool ok = false;

    if (is_flate) {
        size_t plen = 0;
        if (tspdf_stream_decode(dict, stored, stored_len, &flate_pixels, &plen) ==
                TSPDF_OK &&
            plen >= (size_t)w * h * (unsigned)comps) {
            pixels = flate_pixels;
            ok = true;
        }
    } else {
        TspdfRawImage img = {0};
        if (tspdf_jpeg_decode(stored, stored_len, &work, &img) &&
            img.width == (int)w && img.height == (int)h &&
            img.components == comps) {
            pixels = img.pixels;
            ok = true;
        }
        // progressive/arithmetic/CMYK or mismatched JPEG: pass through
    }

    bool replaced = false;
    if (ok) {
        uint8_t *down = (uint8_t *)tspdf_arena_alloc(&work,
                                                     (size_t)dw * dh * (unsigned)comps);
        // On downsample failure (allocation) keep the original image: `down`
        // holds uninitialized arena bytes and must never reach the encoder.
        if (down && tspdf_lossy_box_downsample(pixels, w, h, comps, down, dw, dh)) {
            TspdfRawImage out_img;
            out_img.width = (int)dw;
            out_img.height = (int)dh;
            out_img.components = comps;
            out_img.pixels = down;

            bool gray = comps == 1;
            if (comps == 3 && tspdf_lossy_rgb_is_gray(down, (size_t)dw * dh)) {
                uint8_t *g = (uint8_t *)tspdf_arena_alloc(&work, (size_t)dw * dh);
                if (g) {
                    tspdf_lossy_rgb_to_gray(down, (size_t)dw * dh, g);
                    out_img.components = 1;
                    out_img.pixels = g;
                    gray = true;
                }
            }

            uint8_t *jpeg = NULL;
            size_t jpeg_len = 0;
            if (tspdf_jpeg_encode(&out_img, quality, &work, &jpeg, &jpeg_len) &&
                tspdf_lossy_worth_replacing(stored_len, jpeg_len)) {
                uint8_t *copy = (uint8_t *)malloc(jpeg_len);
                if (copy && lossy_rewrite_dict(doc, dict, dw, dh, gray, jpeg_len)) {
                    memcpy(copy, jpeg, jpeg_len);
                    free(obj->stream.data);  // drop any cached decode
                    obj->stream.data = copy;
                    obj->stream.len = jpeg_len;
                    obj->stream.self_contained = true;
                    stats->bytes_before += stored_len;
                    stats->bytes_after += jpeg_len;
                    replaced = true;
                } else {
                    free(copy);
                }
            }
        }
    }

    free(flate_pixels);
    tspdf_arena_destroy(&work);
    free(owned);
    return replaced;
}

TspdfError tspdf_reader_lossy_images(TspdfReader *doc, int target_dpi,
                                     int quality, TspdfLossyStats *stats) {
    TspdfLossyStats local = {0};
    if (stats) *stats = local;
    if (!doc || target_dpi <= 0 || quality < 1 || quality > 100)
        return TSPDF_ERR_INVALID_ARG;

    TspdfLossyPlacement *placements = NULL;
    size_t n = 0;
    TspdfError err = tspdf_lossy_scan_placements(doc, &placements, &n);
    if (err != TSPDF_OK) return err;

    LossyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.doc = doc;
    tspdf_parser_init(&ctx.rp, doc->data, doc->data_len, &doc->arena);
    ctx.scratch = tspdf_arena_create(1 << 12);

    for (size_t i = 0; i < n; i++) {
        if (lossy_process_image(&ctx, placements[i].obj_num, placements[i].w_pt,
                                placements[i].h_pt, target_dpi, quality, &local)) {
            local.images_recompressed++;
            doc->modified = true;
        } else {
            local.images_skipped++;
        }
    }

    tspdf_arena_destroy(&ctx.scratch);
    free(placements);
    if (stats) *stats = local;
    return TSPDF_OK;
}
