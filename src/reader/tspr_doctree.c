// Document-level tree preservation across merge and extract.
//
// Round 1 made extract drop /Outlines, /AcroForm and the destination trees
// wholesale so the output would not pull the source's whole object graph
// back in. This file restores what belongs to the kept pages:
//
//   - Outlines: an item survives iff its destination page survives or any
//     descendant does. Kept items are rebuilt (Title/C/F + relinked
//     First/Last/Prev/Next/Parent/Count); named destinations are flattened
//     to explicit dest arrays so no /Names tree is needed in the output.
//   - AcroForm: fields survive iff a widget of theirs sits on a kept page;
//     /Kids arrays are pruned, /DR /DA /NeedAppearances carry through.
//
// Rebuilt objects are installed into the destination's obj_cache at the
// item's own object number, so sibling/parent references by number keep
// working and dropped items become unreachable (the serializer collects
// only reachable objects — the round-1 size win survives).

#include "tspr_doctree.h"
#include <stdlib.h>
#include <string.h>

// --- small object builders (all allocations in the destination arena) ---

static TspdfObj *dt_obj_new(TspdfArena *a, TspdfObjType type) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = type;
    return o;
}

static TspdfObj *dt_obj_ref(TspdfArena *a, uint32_t num) {
    TspdfObj *o = dt_obj_new(a, TSPDF_OBJ_REF);
    if (o) o->ref.num = num;
    return o;
}

static TspdfObj *dt_obj_int(TspdfArena *a, int64_t v) {
    TspdfObj *o = dt_obj_new(a, TSPDF_OBJ_INT);
    if (o) o->integer = v;
    return o;
}

static TspdfObj *dt_obj_bool(TspdfArena *a, bool v) {
    TspdfObj *o = dt_obj_new(a, TSPDF_OBJ_BOOL);
    if (o) o->boolean = v;
    return o;
}

static TspdfObj *dt_obj_name(TspdfArena *a, const char *name) {
    TspdfObj *o = dt_obj_new(a, TSPDF_OBJ_NAME);
    if (!o) return NULL;
    size_t len = strlen(name);
    o->string.data = (uint8_t *)tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    memcpy(o->string.data, name, len + 1);
    o->string.len = len;
    return o;
}

// Append or replace `key` in an arena-backed dict. Non-static: tspr_attach.c
// builds its Filespec/name-tree dicts with the same helper.
TspdfError tspdf_obj_dict_put(TspdfObj *dict, const char *key, TspdfObj *value,
                              TspdfArena *a) {
    if (!dict || dict->type != TSPDF_OBJ_DICT || !value) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            dict->dict.entries[i].value = value;
            return TSPDF_OK;
        }
    }
    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(a,
        sizeof(TspdfDictEntry) * (dict->dict.count + 1));
    if (!entries) return TSPDF_ERR_ALLOC;
    if (dict->dict.count > 0) {
        memcpy(entries, dict->dict.entries, sizeof(TspdfDictEntry) * dict->dict.count);
    }
    size_t klen = strlen(key);
    char *kcopy = (char *)tspdf_arena_alloc(a, klen + 1);
    if (!kcopy) return TSPDF_ERR_ALLOC;
    memcpy(kcopy, key, klen + 1);
    entries[dict->dict.count].key = kcopy;
    entries[dict->dict.count].value = value;
    dict->dict.entries = entries;
    dict->dict.count++;
    return TSPDF_OK;
}

// Short local alias (historical name; the body moved to tspdf_obj_dict_put).
static TspdfError dt_dict_put(TspdfObj *dict, const char *key, TspdfObj *value,
                              TspdfArena *a) {
    return tspdf_obj_dict_put(dict, key, value, a);
}

// Length-checked name compare: parsed names can carry embedded NULs via
// #00 escapes, so strcmp against string.data would match too eagerly.
static bool dt_name_is(const TspdfObj *o, const char *name) {
    size_t len = strlen(name);
    return o && o->type == TSPDF_OBJ_NAME && o->string.len == len &&
           memcmp(o->string.data, name, len) == 0;
}

// --- resolution ---

// Resolve an object number through `doc`. Merged documents have no backing
// buffer; for them only cached objects and registered new objects resolve.
// Non-static (declared in tspr_doctree.h): tspr_attach.c resolves through the
// same numbering, including registered new objects.
TspdfObj *tspdf_doctree_resolve_num(TspdfReader *doc, TspdfParser *parser, uint32_t num) {
    if (num < doc->xref.count) {
        if (doc->obj_cache[num]) return doc->obj_cache[num];
        if (!doc->data) return NULL;
        return tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache, doc->crypt);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
}

TspdfObj *tspdf_doctree_resolve(TspdfReader *doc, TspdfParser *parser, TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_REF) return obj;
    return tspdf_doctree_resolve_num(doc, parser, obj->ref.num);
}

// Short local aliases for the exported resolvers.
static TspdfObj *dt_resolve_num(TspdfReader *doc, TspdfParser *parser, uint32_t num) {
    return tspdf_doctree_resolve_num(doc, parser, num);
}

static TspdfObj *dt_resolve(TspdfReader *doc, TspdfParser *parser, TspdfObj *obj) {
    return tspdf_doctree_resolve(doc, parser, obj);
}

// The catalog of a source document (merged documents used as merge input
// carry their catalog directly; opened documents also have the trailer Root).
static TspdfObj *dt_source_catalog(TspdfReader *src, TspdfParser *parser) {
    if (src->catalog && src->catalog->type == TSPDF_OBJ_DICT) return src->catalog;
    if (!src->xref.trailer) return NULL;
    TspdfObj *root = dt_resolve(src, parser, tspdf_dict_get(src->xref.trailer, "Root"));
    return root && root->type == TSPDF_OBJ_DICT ? root : NULL;
}

// Add `offset` to every ref in an object copied from a source document into
// merged numbering. Copies are trees (deep_copy does not follow refs), so
// plain bounded recursion is safe.
static void dt_remap_refs(TspdfObj *obj, uint32_t offset, size_t src_count, int depth) {
    if (!obj || depth > 300) return;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            if (obj->ref.num < src_count) obj->ref.num += offset;
            break;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                dt_remap_refs(&obj->array.items[i], offset, src_count, depth + 1);
            }
            break;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                dt_remap_refs(obj->dict.entries[i].value, offset, src_count, depth + 1);
            }
            break;
        case TSPDF_OBJ_STREAM:
            dt_remap_refs(obj->stream.dict, offset, src_count, depth + 1);
            break;
        default:
            break;
    }
}

// --- named destination lookup (in the source document's numbering) ---

// `budget` caps the total nodes visited per walk (same guard idea as
// DtCtx.budget): /Kids full of self-references would otherwise fan out to
// count^depth recursive calls under the depth cap alone. Non-static: this is
// the one name-tree walker; the destination lookup below and the attachment
// enumeration in tspr_attach.c are both built on it.
bool tspdf_nametree_walk(TspdfReader *doc, TspdfParser *parser, TspdfObj *node,
                         int depth, size_t *budget,
                         TspdfNametreeVisitFn visit, void *ctx) {
    if (!node || node->type != TSPDF_OBJ_DICT || depth > 32) return true;
    if (*budget == 0) return false;
    (*budget)--;
    TspdfObj *names = dt_resolve(doc, parser, tspdf_dict_get(node, "Names"));
    if (names && names->type == TSPDF_OBJ_ARRAY) {
        for (size_t i = 0; i + 1 < names->array.count; i += 2) {
            if (!visit(ctx, &names->array.items[i], &names->array.items[i + 1])) {
                return false;
            }
        }
    }
    TspdfObj *kids = dt_resolve(doc, parser, tspdf_dict_get(node, "Kids"));
    if (kids && kids->type == TSPDF_OBJ_ARRAY) {
        for (size_t i = 0; i < kids->array.count; i++) {
            TspdfObj *kid = dt_resolve(doc, parser, &kids->array.items[i]);
            if (!tspdf_nametree_walk(doc, parser, kid, depth + 1, budget,
                                     visit, ctx)) {
                return false;
            }
        }
    }
    return true;
}

typedef struct {
    const uint8_t *name;
    size_t name_len;
    TspdfObj *hit;
} DtLookupCtx;

static bool dt_lookup_visit(void *vctx, TspdfObj *key, TspdfObj *value) {
    DtLookupCtx *c = (DtLookupCtx *)vctx;
    if (key->type == TSPDF_OBJ_STRING && key->string.len == c->name_len &&
        memcmp(key->string.data, c->name, c->name_len) == 0) {
        c->hit = value;
        return false;
    }
    return true;
}

static TspdfObj *dt_nametree_lookup(TspdfReader *doc, TspdfParser *parser,
                                    TspdfObj *node, const uint8_t *name,
                                    size_t name_len, int depth, size_t *budget) {
    DtLookupCtx c = {name, name_len, NULL};
    tspdf_nametree_walk(doc, parser, node, depth, budget, dt_lookup_visit, &c);
    return c.hit;
}

// --- outline pruning ---

typedef enum {
    DT_TARGET_NONE,     // item has no page destination (e.g. URI action)
    DT_TARGET_VALID,    // destination resolves to a kept page
    DT_TARGET_INVALID,  // destination broken or page not kept
} DtTarget;

typedef struct {
    TspdfReader *doc;        // document holding the items being pruned
    TspdfParser *parser;     // parser over doc's buffer (may back no data)
    TspdfReader *name_doc;   // source document for named-dest lookup
    TspdfParser *name_parser;
    TspdfObj *dests_dict;    // name_doc catalog /Dests (PDF 1.1), resolved
    TspdfObj *names_root;    // name_doc /Names -> /Dests tree root, resolved
    uint32_t ref_offset;     // name_doc numbering + ref_offset = doc numbering
    const bool *page_kept;   // by doc object number
    TspdfArena *arena;       // rebuilt objects land here
    bool merge_mode;         // merge keeps items without a page target
    size_t budget;           // remaining items to visit (cycle guard)
} DtCtx;

// Resolve an outline destination (explicit array, named string/name, or dest
// dict with /D) to an explicit dest array in doc numbering, validating that
// its first element references a kept page.
static DtTarget dt_flatten_dest(DtCtx *ctx, TspdfObj *dest_val, TspdfObj **out_array) {
    *out_array = NULL;
    if (!dest_val) return DT_TARGET_NONE;

    TspdfObj *dest = dt_resolve(ctx->doc, ctx->parser, dest_val);
    if (!dest) return DT_TARGET_INVALID;

    bool from_name_doc = false;
    if (dest->type == TSPDF_OBJ_STRING || dest->type == TSPDF_OBJ_NAME) {
        TspdfObj *val = NULL;
        if (ctx->dests_dict && ctx->dests_dict->type == TSPDF_OBJ_DICT) {
            for (size_t i = 0; i < ctx->dests_dict->dict.count; i++) {
                const char *key = ctx->dests_dict->dict.entries[i].key;
                if (strlen(key) == dest->string.len &&
                    memcmp(key, dest->string.data, dest->string.len) == 0) {
                    val = ctx->dests_dict->dict.entries[i].value;
                    break;
                }
            }
        }
        if (!val && ctx->names_root) {
            // A legitimate lookup visits at most one node per object; anything
            // past that is cyclic /Kids re-visiting.
            size_t budget = ctx->name_doc->xref.count + 16;
            val = dt_nametree_lookup(ctx->name_doc, ctx->name_parser,
                                     ctx->names_root, dest->string.data,
                                     dest->string.len, 0, &budget);
        }
        if (!val) return DT_TARGET_INVALID;
        dest = dt_resolve(ctx->name_doc, ctx->name_parser, val);
        from_name_doc = true;
        if (!dest) return DT_TARGET_INVALID;
    }

    if (dest->type == TSPDF_OBJ_DICT) {
        // Full destination dict form: << /D [...] >>
        TspdfReader *rdoc = from_name_doc ? ctx->name_doc : ctx->doc;
        TspdfParser *rparser = from_name_doc ? ctx->name_parser : ctx->parser;
        dest = dt_resolve(rdoc, rparser, tspdf_dict_get(dest, "D"));
        if (!dest) return DT_TARGET_INVALID;
    }

    if (dest->type != TSPDF_OBJ_ARRAY || dest->array.count == 0) {
        return DT_TARGET_INVALID;
    }
    TspdfObj *first = &dest->array.items[0];
    if (first->type != TSPDF_OBJ_REF) return DT_TARGET_INVALID;

    uint32_t page_num = first->ref.num;
    if (from_name_doc) {
        if (page_num >= ctx->name_doc->xref.count) return DT_TARGET_INVALID;
        page_num += ctx->ref_offset;
    }
    if (page_num >= ctx->doc->xref.count || !ctx->page_kept[page_num]) {
        return DT_TARGET_INVALID;
    }

    TspdfObj *copy = tspdf_obj_deep_copy(dest, ctx->arena);
    if (!copy) return DT_TARGET_INVALID;
    if (from_name_doc && ctx->ref_offset > 0) {
        dt_remap_refs(copy, ctx->ref_offset, ctx->name_doc->xref.count, 0);
    }
    *out_array = copy;
    return DT_TARGET_VALID;
}

typedef struct {
    uint32_t *nums;
    size_t count;
    size_t cap;
} DtNumList;

static bool dt_numlist_push(DtNumList *l, uint32_t num) {
    if (l->count >= l->cap) {
        size_t cap = l->cap == 0 ? 16 : l->cap * 2;
        uint32_t *grown = (uint32_t *)realloc(l->nums, cap * sizeof(uint32_t));
        if (!grown) return false;
        l->nums = grown;
        l->cap = cap;
    }
    l->nums[l->count++] = num;
    return true;
}

static bool dt_numlist_contains(const DtNumList *l, uint32_t num) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->nums[i] == num) return true;
    }
    return false;
}

// Copy `key` from `item` into `copy` verbatim (deep copy) when present.
static TspdfError dt_copy_key(TspdfObj *copy, TspdfObj *item, const char *key,
                              TspdfArena *a) {
    TspdfObj *val = tspdf_dict_get(item, key);
    if (!val) return TSPDF_OK;
    TspdfObj *vcopy = tspdf_obj_deep_copy(val, a);
    if (!vcopy) return TSPDF_ERR_ALLOC;
    return dt_dict_put(copy, key, vcopy, a);
}

// Walk one sibling chain, pruning recursively. Survivor numbers append to
// `survivors`; kept items are rebuilt and installed into doc->obj_cache at
// their own number (so /Parent refs from children keep resolving). The
// caller relinks /Prev//Next once the full level is known.
static TspdfError dt_prune_outline_level(DtCtx *ctx, TspdfObj *first_val,
                                         int depth, DtNumList *survivors) {
    if (depth > 64) return TSPDF_OK;

    TspdfObj *cur = first_val;
    while (cur && cur->type == TSPDF_OBJ_REF && ctx->budget > 0) {
        ctx->budget--;
        uint32_t num = cur->ref.num;
        // A number already kept at this level means /Next looped back; the
        // rest of the chain retraces itself, and pushing the duplicate would
        // re-emit the cycle as a /Prev//Next loop with an inflated /Count.
        if (dt_numlist_contains(survivors, num)) break;
        TspdfObj *item = dt_resolve_num(ctx->doc, ctx->parser, num);
        if (!item || item->type != TSPDF_OBJ_DICT) break;

        TspdfObj *next_val = tspdf_dict_get(item, "Next");

        DtNumList kids = {0};
        TspdfError err = dt_prune_outline_level(ctx, tspdf_dict_get(item, "First"),
                                                depth + 1, &kids);
        if (err != TSPDF_OK) {
            free(kids.nums);
            return err;
        }

        // Page target: /Dest wins over /A; a /GoTo action contributes its /D.
        TspdfObj *dest_val = tspdf_dict_get(item, "Dest");
        TspdfObj *action = dt_resolve(ctx->doc, ctx->parser, tspdf_dict_get(item, "A"));
        bool action_is_goto = false;
        if (action && action->type == TSPDF_OBJ_DICT) {
            action_is_goto = dt_name_is(tspdf_dict_get(action, "S"), "GoTo");
            if (!dest_val && action_is_goto) {
                dest_val = tspdf_dict_get(action, "D");
            }
        }

        TspdfObj *flat = NULL;
        DtTarget target = dt_flatten_dest(ctx, dest_val, &flat);

        // Merge keeps everything not pointing at a missing page (URI items
        // copy through); extract keeps only what resolves to a kept page.
        bool self_keep = ctx->merge_mode ? (target != DT_TARGET_INVALID)
                                         : (target == DT_TARGET_VALID);
        if (self_keep || kids.count > 0) {
            TspdfObj *copy = dt_obj_new(ctx->arena, TSPDF_OBJ_DICT);
            if (!copy) {
                free(kids.nums);
                return TSPDF_ERR_ALLOC;
            }
            err = dt_copy_key(copy, item, "Title", ctx->arena);
            if (err == TSPDF_OK) err = dt_copy_key(copy, item, "C", ctx->arena);
            if (err == TSPDF_OK) err = dt_copy_key(copy, item, "F", ctx->arena);
            if (err == TSPDF_OK && flat) {
                err = dt_dict_put(copy, "Dest", flat, ctx->arena);
            }
            if (err == TSPDF_OK && action && !action_is_goto) {
                TspdfObj *acopy = tspdf_obj_deep_copy(action, ctx->arena);
                if (!acopy) err = TSPDF_ERR_ALLOC;
                else err = dt_dict_put(copy, "A", acopy, ctx->arena);
            }
            // Keep the parent by number: a kept parent is installed at the
            // same number. Top-level items get re-parented by the caller.
            TspdfObj *parent = tspdf_dict_get(item, "Parent");
            if (err == TSPDF_OK && parent && parent->type == TSPDF_OBJ_REF) {
                TspdfObj *pref = dt_obj_ref(ctx->arena, parent->ref.num);
                if (!pref) err = TSPDF_ERR_ALLOC;
                else err = dt_dict_put(copy, "Parent", pref, ctx->arena);
            }
            if (err == TSPDF_OK && kids.count > 0) {
                TspdfObj *fref = dt_obj_ref(ctx->arena, kids.nums[0]);
                TspdfObj *lref = dt_obj_ref(ctx->arena, kids.nums[kids.count - 1]);
                int64_t cnt = (int64_t)kids.count;
                TspdfObj *ocount = tspdf_dict_get(item, "Count");
                if (ocount && ocount->type == TSPDF_OBJ_INT && ocount->integer < 0) {
                    cnt = -cnt;  // preserve closed state
                }
                TspdfObj *cobj = dt_obj_int(ctx->arena, cnt);
                if (!fref || !lref || !cobj) err = TSPDF_ERR_ALLOC;
                if (err == TSPDF_OK) err = dt_dict_put(copy, "First", fref, ctx->arena);
                if (err == TSPDF_OK) err = dt_dict_put(copy, "Last", lref, ctx->arena);
                if (err == TSPDF_OK) err = dt_dict_put(copy, "Count", cobj, ctx->arena);
            }
            if (err != TSPDF_OK) {
                free(kids.nums);
                return err;
            }
            if (num < ctx->doc->xref.count) {
                ctx->doc->obj_cache[num] = copy;
                if (!dt_numlist_push(survivors, num)) {
                    free(kids.nums);
                    return TSPDF_ERR_ALLOC;
                }
            }
        }
        free(kids.nums);
        cur = next_val;
    }

    // Relink the surviving siblings. At the top level of a merge this runs
    // once per source over the accumulated list; the rewrites are idempotent.
    for (size_t i = 0; i < survivors->count; i++) {
        uint32_t num = survivors->nums[i];
        TspdfObj *it = num < ctx->doc->xref.count ? ctx->doc->obj_cache[num] : NULL;
        if (!it) continue;
        if (i > 0) {
            TspdfObj *ref = dt_obj_ref(ctx->arena, survivors->nums[i - 1]);
            if (!ref || dt_dict_put(it, "Prev", ref, ctx->arena) != TSPDF_OK) {
                return TSPDF_ERR_ALLOC;
            }
        }
        if (i + 1 < survivors->count) {
            TspdfObj *ref = dt_obj_ref(ctx->arena, survivors->nums[i + 1]);
            if (!ref || dt_dict_put(it, "Next", ref, ctx->arena) != TSPDF_OK) {
                return TSPDF_ERR_ALLOC;
            }
        }
    }
    return TSPDF_OK;
}

// Register a fresh /Outlines root over `top` and point catalog /Outlines at
// it. No-op when nothing survived.
static TspdfError dt_attach_outline_root(TspdfReader *doc, TspdfArena *arena,
                                         DtNumList *top, TspdfObj *catalog) {
    if (top->count == 0) return TSPDF_OK;

    TspdfObj *root = dt_obj_new(arena, TSPDF_OBJ_DICT);
    TspdfObj *type = dt_obj_name(arena, "Outlines");
    TspdfObj *fref = dt_obj_ref(arena, top->nums[0]);
    TspdfObj *lref = dt_obj_ref(arena, top->nums[top->count - 1]);
    TspdfObj *cnt = dt_obj_int(arena, (int64_t)top->count);
    if (!root || !type || !fref || !lref || !cnt) return TSPDF_ERR_ALLOC;
    TspdfError err = dt_dict_put(root, "Type", type, arena);
    if (err == TSPDF_OK) err = dt_dict_put(root, "First", fref, arena);
    if (err == TSPDF_OK) err = dt_dict_put(root, "Last", lref, arena);
    if (err == TSPDF_OK) err = dt_dict_put(root, "Count", cnt, arena);
    if (err != TSPDF_OK) return err;

    uint32_t root_num = tspdf_register_new_obj(doc, root);
    if (root_num == 0) return TSPDF_ERR_ALLOC;

    for (size_t i = 0; i < top->count; i++) {
        uint32_t num = top->nums[i];
        TspdfObj *it = num < doc->xref.count ? doc->obj_cache[num] : NULL;
        if (!it) continue;
        TspdfObj *pref = dt_obj_ref(arena, root_num);
        if (!pref || dt_dict_put(it, "Parent", pref, arena) != TSPDF_OK) {
            return TSPDF_ERR_ALLOC;
        }
    }

    TspdfObj *rref = dt_obj_ref(arena, root_num);
    if (!rref) return TSPDF_ERR_ALLOC;
    return dt_dict_put(catalog, "Outlines", rref, arena);
}

// --- AcroForm pruning (extract) ---

// Map annotation object number -> page object number across all source
// pages, for widgets without /P.
static void dt_build_annot_page_map(TspdfReader *src, TspdfParser *parser,
                                    uint32_t *map) {
    for (size_t p = 0; p < src->pages.count; p++) {
        TspdfReaderPage *page = &src->pages.pages[p];
        TspdfObj *annots = dt_resolve(src, parser,
                                      tspdf_dict_get(page->page_dict, "Annots"));
        if (!annots || annots->type != TSPDF_OBJ_ARRAY) continue;
        for (size_t i = 0; i < annots->array.count; i++) {
            TspdfObj *ref = &annots->array.items[i];
            if (ref->type == TSPDF_OBJ_REF && ref->ref.num < src->xref.count &&
                map[ref->ref.num] == 0) {
                map[ref->ref.num] = page->obj_num;
            }
        }
    }
}

// True when the field keeps at least one widget on a kept page. Fields with
// /Kids are rebuilt with the surviving kids and installed into dst's cache;
// terminal fields (the field is the widget) pass through untouched.
// `budget` caps the total nodes visited across the whole field forest (a
// legitimate tree visits each object at most once); without it, /Kids full
// of self-references fan out to count^depth calls under the depth cap alone.
static bool dt_prune_field(TspdfReader *dst, TspdfParser *parser,
                           const bool *page_kept, const uint32_t *annot_page,
                           uint32_t field_num, int depth, size_t *budget) {
    if (depth > 32 || field_num >= dst->xref.count) return false;
    if (*budget == 0) return false;
    (*budget)--;
    TspdfObj *field = dt_resolve_num(dst, parser, field_num);
    if (!field || field->type != TSPDF_OBJ_DICT) return false;

    TspdfObj *kids = dt_resolve(dst, parser, tspdf_dict_get(field, "Kids"));
    if (kids && kids->type == TSPDF_OBJ_ARRAY && kids->array.count > 0) {
        DtNumList kept = {0};
        for (size_t i = 0; i < kids->array.count; i++) {
            TspdfObj *kid = &kids->array.items[i];
            if (kid->type != TSPDF_OBJ_REF) continue;
            if (dt_prune_field(dst, parser, page_kept, annot_page,
                               kid->ref.num, depth + 1, budget)) {
                if (!dt_numlist_push(&kept, kid->ref.num)) {
                    free(kept.nums);
                    return false;
                }
            }
        }
        if (kept.count == 0) {
            free(kept.nums);
            return false;
        }

        // Rebuild the field with the pruned /Kids array.
        TspdfObj *copy = dt_obj_new(&dst->arena, TSPDF_OBJ_DICT);
        if (!copy) {
            free(kept.nums);
            return false;
        }
        TspdfObj *karr = dt_obj_new(&dst->arena, TSPDF_OBJ_ARRAY);
        if (!karr) {
            free(kept.nums);
            return false;
        }
        karr->array.count = kept.count;
        karr->array.items = (TspdfObj *)tspdf_arena_alloc(&dst->arena,
            sizeof(TspdfObj) * kept.count);
        if (!karr->array.items) {
            free(kept.nums);
            return false;
        }
        for (size_t i = 0; i < kept.count; i++) {
            memset(&karr->array.items[i], 0, sizeof(TspdfObj));
            karr->array.items[i].type = TSPDF_OBJ_REF;
            karr->array.items[i].ref.num = kept.nums[i];
        }
        free(kept.nums);

        for (size_t i = 0; i < field->dict.count; i++) {
            const char *key = field->dict.entries[i].key;
            if (strcmp(key, "Kids") == 0) continue;
            if (dt_copy_key(copy, field, key, &dst->arena) != TSPDF_OK) return false;
        }
        if (dt_dict_put(copy, "Kids", karr, &dst->arena) != TSPDF_OK) return false;
        dst->obj_cache[field_num] = copy;
        return true;
    }

    // Terminal field: it is its own widget; find its page.
    uint32_t page_num = 0;
    TspdfObj *p = tspdf_dict_get(field, "P");
    if (p && p->type == TSPDF_OBJ_REF) {
        page_num = p->ref.num;
    } else if (annot_page && field_num < dst->xref.count) {
        page_num = annot_page[field_num];
    }
    // page_num == 0 doubles as the not-found sentinel; that is safe only
    // because object 0 is the xref free-list head and can never be a page.
    return page_num > 0 && page_num < dst->xref.count && page_kept[page_num];
}

// Build an AcroForm dict from a fields ref list plus carried-over entries,
// register it, and point catalog /AcroForm at it.
static TspdfError dt_attach_acroform(TspdfReader *doc, TspdfArena *arena,
                                     const DtNumList *fields, TspdfObj *dr,
                                     TspdfObj *da, bool need_appearances,
                                     TspdfObj *catalog) {
    if (fields->count == 0) return TSPDF_OK;

    TspdfObj *farr = dt_obj_new(arena, TSPDF_OBJ_ARRAY);
    if (!farr) return TSPDF_ERR_ALLOC;
    farr->array.count = fields->count;
    farr->array.items = (TspdfObj *)tspdf_arena_alloc(arena,
        sizeof(TspdfObj) * fields->count);
    if (!farr->array.items) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < fields->count; i++) {
        memset(&farr->array.items[i], 0, sizeof(TspdfObj));
        farr->array.items[i].type = TSPDF_OBJ_REF;
        farr->array.items[i].ref.num = fields->nums[i];
    }

    TspdfObj *acro = dt_obj_new(arena, TSPDF_OBJ_DICT);
    if (!acro) return TSPDF_ERR_ALLOC;
    TspdfError err = dt_dict_put(acro, "Fields", farr, arena);
    if (err == TSPDF_OK && dr) err = dt_dict_put(acro, "DR", dr, arena);
    if (err == TSPDF_OK && da) err = dt_dict_put(acro, "DA", da, arena);
    if (err == TSPDF_OK && need_appearances) {
        TspdfObj *na = dt_obj_bool(arena, true);
        if (!na) return TSPDF_ERR_ALLOC;
        err = dt_dict_put(acro, "NeedAppearances", na, arena);
    }
    if (err != TSPDF_OK) return err;

    uint32_t acro_num = tspdf_register_new_obj(doc, acro);
    if (acro_num == 0) return TSPDF_ERR_ALLOC;
    TspdfObj *aref = dt_obj_ref(arena, acro_num);
    if (!aref) return TSPDF_ERR_ALLOC;
    return dt_dict_put(catalog, "AcroForm", aref, arena);
}

// Merge second-source /DR entries into the accumulated one: whole entries
// copy when missing; when both sides have a dict for the same key (the
// /Font dict, typically) the sub-entries merge additively with the first
// source winning name collisions.
static TspdfError dt_merge_dr(TspdfObj *dr, TspdfObj *other, TspdfArena *arena) {
    for (size_t i = 0; i < other->dict.count; i++) {
        const char *key = other->dict.entries[i].key;
        TspdfObj *have = tspdf_dict_get(dr, key);
        if (!have) {
            TspdfError err = dt_dict_put(dr, key, other->dict.entries[i].value, arena);
            if (err != TSPDF_OK) return err;
            continue;
        }
        TspdfObj *val = other->dict.entries[i].value;
        if (have->type != TSPDF_OBJ_DICT || !val || val->type != TSPDF_OBJ_DICT) {
            continue;  // first source wins
        }
        for (size_t j = 0; j < val->dict.count; j++) {
            if (tspdf_dict_get(have, val->dict.entries[j].key)) continue;
            TspdfError err = dt_dict_put(have, val->dict.entries[j].key,
                                         val->dict.entries[j].value, arena);
            if (err != TSPDF_OK) return err;
        }
    }
    return TSPDF_OK;
}

// --- merge entry points ---

// Resolve the outline items of one level into the source cache; dest arrays
// and actions get their streams self-contained. /SE and /Parent are not
// walked (/SE would pull the structure tree; parents are covered by the
// level walk itself).
static void dt_prepare_outline_level(TspdfReader *src, TspdfParser *parser,
                                     bool *visited, TspdfObj *first_val,
                                     int depth, size_t *budget) {
    if (depth > 64) return;
    TspdfObj *cur = first_val;
    while (cur && cur->type == TSPDF_OBJ_REF && *budget > 0) {
        (*budget)--;
        uint32_t num = cur->ref.num;
        if (num >= src->xref.count) break;
        TspdfObj *item = tspdf_xref_resolve(&src->xref, parser, num,
                                            src->obj_cache, src->crypt);
        if (!item || item->type != TSPDF_OBJ_DICT) break;
        visited[num] = true;

        TspdfObj *dest = tspdf_dict_get(item, "Dest");
        if (dest) {
            tspdf_reader_make_streams_self_contained(dest, src->data, src->data_len,
                src->obj_cache, &src->xref, parser, src->crypt, visited,
                src->xref.count);
        }
        TspdfObj *action = tspdf_dict_get(item, "A");
        if (action) {
            tspdf_reader_make_streams_self_contained(action, src->data, src->data_len,
                src->obj_cache, &src->xref, parser, src->crypt, visited,
                src->xref.count);
        }

        dt_prepare_outline_level(src, parser, visited,
                                 tspdf_dict_get(item, "First"), depth + 1, budget);
        cur = tspdf_dict_get(item, "Next");
    }
}

TspdfError tspdf_doctree_merge_prepare(TspdfReader *src, TspdfParser *parser,
                                       bool *visited) {
    TspdfObj *catalog = dt_source_catalog(src, parser);
    if (!catalog) return TSPDF_OK;

    TspdfObj *outlines_val = tspdf_dict_get(catalog, "Outlines");
    if (outlines_val) {
        TspdfObj *root = dt_resolve(src, parser, outlines_val);
        if (root && root->type == TSPDF_OBJ_DICT) {
            size_t budget = src->xref.count + 16;
            dt_prepare_outline_level(src, parser, visited,
                                     tspdf_dict_get(root, "First"), 0, &budget);
        }
    }

    TspdfObj *acro_val = tspdf_dict_get(catalog, "AcroForm");
    if (acro_val) {
        if (acro_val->type == TSPDF_OBJ_REF && acro_val->ref.num < src->xref.count) {
            TspdfError err = tspdf_reader_make_ref_self_contained(acro_val->ref.num,
                src->data, src->data_len, src->obj_cache, &src->xref, parser,
                src->crypt, visited, src->xref.count);
            if (err != TSPDF_OK) return err;
        } else if (acro_val->type == TSPDF_OBJ_DICT) {
            TspdfError err = tspdf_reader_make_streams_self_contained(acro_val,
                src->data, src->data_len, src->obj_cache, &src->xref, parser,
                src->crypt, visited, src->xref.count);
            if (err != TSPDF_OK) return err;
        }
    }
    return TSPDF_OK;
}

TspdfError tspdf_doctree_merge_attach(TspdfReader *merged, TspdfReader **docs,
                                      size_t count, const size_t *xref_offsets) {
    bool *page_kept = (bool *)calloc(merged->xref.count, sizeof(bool));
    if (!page_kept) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < merged->pages.count; i++) {
        uint32_t num = merged->pages.pages[i].obj_num;
        if (num < merged->xref.count) page_kept[num] = true;
    }

    TspdfParser merged_parser;
    tspdf_parser_init(&merged_parser, merged->data, merged->data_len, &merged->arena);

    DtNumList top = {0};
    DtNumList acro_fields = {0};
    TspdfObj *dr = NULL;
    TspdfObj *da = NULL;
    bool need_appearances = false;
    TspdfError err = TSPDF_OK;

    for (size_t d = 0; d < count && err == TSPDF_OK; d++) {
        TspdfReader *src = docs[d];
        uint32_t offset = (uint32_t)xref_offsets[d];
        TspdfParser src_parser;
        tspdf_parser_init(&src_parser, src->data, src->data_len, &src->arena);

        TspdfObj *src_catalog = dt_source_catalog(src, &src_parser);
        if (!src_catalog) continue;

        // Outlines: chain through the merged copies (all refs remapped by
        // the merge copy loop); only the root->First hop crosses numbering.
        TspdfObj *root = dt_resolve(src, &src_parser,
                                    tspdf_dict_get(src_catalog, "Outlines"));
        if (root && root->type == TSPDF_OBJ_DICT) {
            TspdfObj *first = tspdf_dict_get(root, "First");
            if (first && first->type == TSPDF_OBJ_REF &&
                first->ref.num < src->xref.count) {
                TspdfObj first_merged = {0};
                first_merged.type = TSPDF_OBJ_REF;
                first_merged.ref.num = first->ref.num + offset;

                DtCtx ctx = {0};
                ctx.doc = merged;
                ctx.parser = &merged_parser;
                ctx.name_doc = src;
                ctx.name_parser = &src_parser;
                ctx.dests_dict = dt_resolve(src, &src_parser,
                                            tspdf_dict_get(src_catalog, "Dests"));
                TspdfObj *names = dt_resolve(src, &src_parser,
                                             tspdf_dict_get(src_catalog, "Names"));
                if (names && names->type == TSPDF_OBJ_DICT) {
                    ctx.names_root = dt_resolve(src, &src_parser,
                                                tspdf_dict_get(names, "Dests"));
                }
                ctx.ref_offset = offset;
                ctx.page_kept = page_kept;
                ctx.arena = &merged->arena;
                ctx.merge_mode = true;
                ctx.budget = merged->xref.count + 16;

                err = dt_prune_outline_level(&ctx, &first_merged, 0, &top);
                if (err != TSPDF_OK) break;
            }
        }

        // AcroForm: concatenate the fields; /DR//DA come from the first
        // source that has them (font dicts merge additively, first wins).
        TspdfObj *acro = dt_resolve(src, &src_parser,
                                    tspdf_dict_get(src_catalog, "AcroForm"));
        if (acro && acro->type == TSPDF_OBJ_DICT) {
            TspdfObj *fields = dt_resolve(src, &src_parser,
                                          tspdf_dict_get(acro, "Fields"));
            if (fields && fields->type == TSPDF_OBJ_ARRAY) {
                for (size_t i = 0; i < fields->array.count; i++) {
                    TspdfObj *f = &fields->array.items[i];
                    if (f->type != TSPDF_OBJ_REF || f->ref.num >= src->xref.count) {
                        continue;
                    }
                    uint32_t merged_num = f->ref.num + offset;
                    if (merged_num >= merged->xref.count ||
                        !merged->obj_cache[merged_num]) {
                        continue;
                    }
                    if (!dt_numlist_push(&acro_fields, merged_num)) {
                        err = TSPDF_ERR_ALLOC;
                        break;
                    }
                }
            }
            if (err != TSPDF_OK) break;

            TspdfObj *src_dr = dt_resolve(src, &src_parser,
                                          tspdf_dict_get(acro, "DR"));
            if (src_dr && src_dr->type == TSPDF_OBJ_DICT) {
                TspdfObj *dr_copy = tspdf_obj_deep_copy(src_dr, &merged->arena);
                if (!dr_copy) {
                    err = TSPDF_ERR_ALLOC;
                    break;
                }
                dt_remap_refs(dr_copy, offset, src->xref.count, 0);
                if (!dr) {
                    dr = dr_copy;
                } else {
                    err = dt_merge_dr(dr, dr_copy, &merged->arena);
                    if (err != TSPDF_OK) break;
                }
            }
            if (!da) {
                TspdfObj *src_da = tspdf_dict_get(acro, "DA");
                if (src_da && src_da->type == TSPDF_OBJ_STRING) {
                    da = tspdf_obj_deep_copy(src_da, &merged->arena);
                    if (!da) {
                        err = TSPDF_ERR_ALLOC;
                        break;
                    }
                }
            }
            TspdfObj *na = dt_resolve(src, &src_parser,
                                      tspdf_dict_get(acro, "NeedAppearances"));
            if (na && na->type == TSPDF_OBJ_BOOL && na->boolean) {
                need_appearances = true;
            }
        }
    }

    if (err == TSPDF_OK && (top.count > 0 || acro_fields.count > 0)) {
        merged->catalog = dt_obj_new(&merged->arena, TSPDF_OBJ_DICT);
        if (!merged->catalog) err = TSPDF_ERR_ALLOC;
        if (err == TSPDF_OK) {
            err = dt_attach_outline_root(merged, &merged->arena, &top,
                                         merged->catalog);
        }
        if (err == TSPDF_OK) {
            err = dt_attach_acroform(merged, &merged->arena, &acro_fields, dr, da,
                                     need_appearances, merged->catalog);
        }
    }

    free(top.nums);
    free(acro_fields.nums);
    free(page_kept);
    return err;
}

// --- extract entry point ---

TspdfError tspdf_doctree_extract_attach(TspdfReader *src, TspdfReader *dst) {
    if (!dst->catalog || dst->catalog->type != TSPDF_OBJ_DICT) return TSPDF_OK;

    TspdfParser src_parser;
    tspdf_parser_init(&src_parser, src->data, src->data_len, &src->arena);
    TspdfObj *src_catalog = dt_source_catalog(src, &src_parser);
    if (!src_catalog) return TSPDF_OK;

    // Parses on behalf of the extracted document allocate in its arena.
    TspdfParser dst_parser;
    tspdf_parser_init(&dst_parser, dst->data, dst->data_len, &dst->arena);

    bool *page_kept = (bool *)calloc(dst->xref.count, sizeof(bool));
    if (!page_kept) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < dst->pages.count; i++) {
        uint32_t num = dst->pages.pages[i].obj_num;
        if (num < dst->xref.count) page_kept[num] = true;
    }

    TspdfError err = TSPDF_OK;

    // Outlines.
    TspdfObj *root = dt_resolve(src, &src_parser,
                                tspdf_dict_get(src_catalog, "Outlines"));
    if (root && root->type == TSPDF_OBJ_DICT) {
        DtCtx ctx = {0};
        ctx.doc = dst;
        ctx.parser = &dst_parser;
        ctx.name_doc = src;
        ctx.name_parser = &src_parser;
        ctx.dests_dict = dt_resolve(src, &src_parser,
                                    tspdf_dict_get(src_catalog, "Dests"));
        TspdfObj *names = dt_resolve(src, &src_parser,
                                     tspdf_dict_get(src_catalog, "Names"));
        if (names && names->type == TSPDF_OBJ_DICT) {
            ctx.names_root = dt_resolve(src, &src_parser,
                                        tspdf_dict_get(names, "Dests"));
        }
        ctx.ref_offset = 0;
        ctx.page_kept = page_kept;
        ctx.arena = &dst->arena;
        ctx.merge_mode = false;
        ctx.budget = dst->xref.count + src->new_objs.count + 16;

        DtNumList top = {0};
        err = dt_prune_outline_level(&ctx, tspdf_dict_get(root, "First"), 0, &top);
        if (err == TSPDF_OK) {
            err = dt_attach_outline_root(dst, &dst->arena, &top, dst->catalog);
        }
        free(top.nums);
    }

    // AcroForm.
    if (err == TSPDF_OK) {
        TspdfObj *acro = dt_resolve(src, &src_parser,
                                    tspdf_dict_get(src_catalog, "AcroForm"));
        if (acro && acro->type == TSPDF_OBJ_DICT) {
            TspdfObj *fields = dt_resolve(src, &src_parser,
                                          tspdf_dict_get(acro, "Fields"));
            if (fields && fields->type == TSPDF_OBJ_ARRAY &&
                fields->array.count > 0) {
                uint32_t *annot_page = (uint32_t *)calloc(src->xref.count,
                                                          sizeof(uint32_t));
                if (!annot_page) {
                    err = TSPDF_ERR_ALLOC;
                } else {
                    dt_build_annot_page_map(src, &src_parser, annot_page);

                    DtNumList kept = {0};
                    size_t field_budget = dst->xref.count + 16;
                    for (size_t i = 0; i < fields->array.count; i++) {
                        TspdfObj *f = &fields->array.items[i];
                        if (f->type != TSPDF_OBJ_REF) continue;
                        if (dt_prune_field(dst, &dst_parser, page_kept, annot_page,
                                           f->ref.num, 0, &field_budget)) {
                            if (!dt_numlist_push(&kept, f->ref.num)) {
                                err = TSPDF_ERR_ALLOC;
                                break;
                            }
                        }
                    }
                    if (err == TSPDF_OK && kept.count > 0) {
                        TspdfObj *dr = NULL;
                        TspdfObj *da = NULL;
                        TspdfObj *src_dr = tspdf_dict_get(acro, "DR");
                        if (src_dr) dr = tspdf_obj_deep_copy(src_dr, &dst->arena);
                        TspdfObj *src_da = tspdf_dict_get(acro, "DA");
                        if (src_da) da = tspdf_obj_deep_copy(src_da, &dst->arena);
                        TspdfObj *na = dt_resolve(src, &src_parser,
                                                  tspdf_dict_get(acro, "NeedAppearances"));
                        bool need_appearances = na && na->type == TSPDF_OBJ_BOOL &&
                                                na->boolean;
                        err = dt_attach_acroform(dst, &dst->arena, &kept, dr, da,
                                                 need_appearances, dst->catalog);
                    }
                    free(kept.nums);
                    free(annot_page);
                }
            }
        }
    }

    free(page_kept);
    return err;
}
