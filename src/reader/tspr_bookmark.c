// Outline (bookmark) editing for existing documents.
//
// Enumeration walks the catalog /Outlines tree in pre-order (parents before
// children), resolving each item's destination — explicit /Dest array, named
// destination (via the shared name-tree walker), or /A /GoTo /D — down to a
// 0-based page index. The walk is budget-guarded exactly like the doctree
// pruners: a cyclic /Next//First chain runs out of budget and stops rather
// than looping, and a number already emitted at the current level is not
// re-emitted.
//
// Replacement builds a brand-new /Outlines tree from a flat {level,title,page}
// list: levels define nesting (level N nests under the nearest preceding level
// N-1). Titles reuse the Info-string convention (ASCII literal, non-ASCII
// UTF-16BE with BOM). Every item object is registered as a new object, the
// root is registered, and the catalog is pointed at it; the old tree simply
// becomes unreachable and the serializer drops it.

#include "tspr_internal.h"
#include "tspr_doctree.h"
#include "../util/pdftext.h"
#include <stdlib.h>
#include <string.h>

// --- catalog access ---

// The document catalog, resolving the trailer /Root when doc->catalog is unset
// (opened documents keep it in the trailer; manipulated ones set it directly).
static TspdfObj *bm_catalog(TspdfReader *doc, TspdfParser *parser) {
    if (doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT) return doc->catalog;
    if (!doc->xref.trailer) return NULL;
    TspdfObj *root = tspdf_doctree_resolve(doc, parser,
                                           tspdf_dict_get(doc->xref.trailer, "Root"));
    return root && root->type == TSPDF_OBJ_DICT ? root : NULL;
}

// Map a page object number to its 0-based index, or SIZE_MAX when not a page.
static size_t bm_page_index_of(const TspdfReader *doc, uint32_t obj_num) {
    for (size_t i = 0; i < doc->pages.count; i++) {
        if (doc->pages.pages[i].obj_num == obj_num) return i;
    }
    return (size_t)-1;
}

// --- destination resolution (to a 0-based page index) ---

typedef struct {
    const uint8_t *name;
    size_t name_len;
    TspdfObj *hit;
} BmNameCtx;

static bool bm_name_visit(void *vctx, TspdfObj *key, TspdfObj *value) {
    BmNameCtx *c = (BmNameCtx *)vctx;
    if (key->type == TSPDF_OBJ_STRING && key->string.len == c->name_len &&
        memcmp(key->string.data, c->name, c->name_len) == 0) {
        c->hit = value;
        return false;
    }
    return true;
}

// Resolve an outline item's destination value to a page index. Handles the
// explicit array form, the named-destination form (looked up in catalog
// /Dests and the /Names -> /Dests name tree), and the << /D [...] >> dict.
static size_t bm_resolve_dest(TspdfReader *doc, TspdfParser *parser,
                              TspdfObj *catalog, TspdfObj *dest_val) {
    if (!dest_val) return (size_t)-1;
    TspdfObj *dest = tspdf_doctree_resolve(doc, parser, dest_val);
    if (!dest) return (size_t)-1;

    if (dest->type == TSPDF_OBJ_STRING || dest->type == TSPDF_OBJ_NAME) {
        TspdfObj *val = NULL;
        // PDF 1.1 /Dests name dictionary in the catalog.
        TspdfObj *dests = tspdf_doctree_resolve(doc, parser,
                                                tspdf_dict_get(catalog, "Dests"));
        if (dests && dests->type == TSPDF_OBJ_DICT) {
            for (size_t i = 0; i < dests->dict.count; i++) {
                const char *key = dests->dict.entries[i].key;
                if (strlen(key) == dest->string.len &&
                    memcmp(key, dest->string.data, dest->string.len) == 0) {
                    val = dests->dict.entries[i].value;
                    break;
                }
            }
        }
        // /Names -> /Dests name tree.
        if (!val) {
            TspdfObj *names = tspdf_doctree_resolve(doc, parser,
                                                    tspdf_dict_get(catalog, "Names"));
            if (names && names->type == TSPDF_OBJ_DICT) {
                TspdfObj *root = tspdf_doctree_resolve(doc, parser,
                                                       tspdf_dict_get(names, "Dests"));
                if (root && root->type == TSPDF_OBJ_DICT) {
                    BmNameCtx c = {dest->string.data, dest->string.len, NULL};
                    size_t budget = doc->xref.count + 16;
                    tspdf_nametree_walk(doc, parser, root, 0, &budget,
                                        bm_name_visit, &c);
                    val = c.hit;
                }
            }
        }
        if (!val) return (size_t)-1;
        dest = tspdf_doctree_resolve(doc, parser, val);
        if (!dest) return (size_t)-1;
    }

    if (dest->type == TSPDF_OBJ_DICT) {
        dest = tspdf_doctree_resolve(doc, parser, tspdf_dict_get(dest, "D"));
        if (!dest) return (size_t)-1;
    }
    if (dest->type != TSPDF_OBJ_ARRAY || dest->array.count == 0) return (size_t)-1;
    TspdfObj *first = &dest->array.items[0];
    if (first->type != TSPDF_OBJ_REF) return (size_t)-1;
    return bm_page_index_of(doc, first->ref.num);
}

// Effective destination value of an outline item: /Dest wins over /A; a /GoTo
// action contributes its /D.
static TspdfObj *bm_item_dest(TspdfReader *doc, TspdfParser *parser, TspdfObj *item) {
    TspdfObj *dest = tspdf_dict_get(item, "Dest");
    if (dest) return dest;
    TspdfObj *action = tspdf_doctree_resolve(doc, parser, tspdf_dict_get(item, "A"));
    if (action && action->type == TSPDF_OBJ_DICT) {
        TspdfObj *s = tspdf_dict_get(action, "S");
        if (s && s->type == TSPDF_OBJ_NAME && s->string.len == 4 &&
            memcmp(s->string.data, "GoTo", 4) == 0) {
            return tspdf_dict_get(action, "D");
        }
    }
    return NULL;
}

// --- enumeration ---

typedef struct {
    TspdfBookmarkInfo *items;
    size_t count;
    size_t cap;
    bool oom;
} BmInfoList;

static bool bm_info_push(BmInfoList *l, TspdfBookmarkInfo info) {
    if (l->count >= l->cap) {
        size_t cap = l->cap == 0 ? 16 : l->cap * 2;
        TspdfBookmarkInfo *grown =
            (TspdfBookmarkInfo *)realloc(l->items, cap * sizeof(TspdfBookmarkInfo));
        if (!grown) { l->oom = true; return false; }
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->count++] = info;
    return true;
}

typedef struct {
    uint32_t *nums;
    size_t count;
    size_t cap;
} BmNumList;

static bool bm_num_push(BmNumList *l, uint32_t num) {
    if (l->count >= l->cap) {
        size_t cap = l->cap == 0 ? 32 : l->cap * 2;
        uint32_t *grown = (uint32_t *)realloc(l->nums, cap * sizeof(uint32_t));
        if (!grown) return false;
        l->nums = grown;
        l->cap = cap;
    }
    l->nums[l->count++] = num;
    return true;
}

static bool bm_num_contains(const BmNumList *l, uint32_t num) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->nums[i] == num) return true;
    }
    return false;
}

// Decode an outline /Title value to arena-owned UTF-8. UTF-16BE-with-BOM
// strings are decoded; plain strings are copied verbatim (NUL-terminated).
static const char *bm_title_utf8(TspdfReader *doc, TspdfObj *item) {
    TspdfObj *t = tspdf_dict_get(item, "Title");
    if (!t || t->type != TSPDF_OBJ_STRING) return "";
    char *utf8 = tspdf_utf16be_to_utf8(t->string.data, t->string.len, &doc->arena);
    if (utf8) return utf8;
    char *copy = (char *)tspdf_arena_alloc(&doc->arena, t->string.len + 1);
    if (!copy) return "";
    if (t->string.len) memcpy(copy, t->string.data, t->string.len);
    copy[t->string.len] = '\0';
    return copy;
}

// Walk one sibling chain, appending items pre-order (parent, then its
// subtree). `seen` records every emitted item number so a cyclic /Next or a
// /First pointing back up the tree is not re-emitted; `budget` caps the total
// items visited across the whole tree.
static void bm_walk_level(TspdfReader *doc, TspdfParser *parser, TspdfObj *catalog,
                          TspdfObj *first_val, int level, int depth,
                          size_t *budget, BmNumList *seen, BmInfoList *out) {
    if (depth > 64) return;
    TspdfObj *cur = first_val;
    while (cur && cur->type == TSPDF_OBJ_REF && *budget > 0) {
        (*budget)--;
        uint32_t num = cur->ref.num;
        if (bm_num_contains(seen, num)) break;
        TspdfObj *item = tspdf_doctree_resolve_num(doc, parser, num);
        if (!item || item->type != TSPDF_OBJ_DICT) break;
        if (!bm_num_push(seen, num)) return;

        TspdfObj *count = tspdf_dict_get(item, "Count");
        bool open = !(count && count->type == TSPDF_OBJ_INT && count->integer < 0);

        TspdfBookmarkInfo info;
        info.title = bm_title_utf8(doc, item);
        info.level = level;
        info.page_index = bm_resolve_dest(doc, parser, catalog,
                                          bm_item_dest(doc, parser, item));
        info.open = open;
        if (!bm_info_push(out, info)) return;

        bm_walk_level(doc, parser, catalog, tspdf_dict_get(item, "First"),
                      level + 1, depth + 1, budget, seen, out);
        cur = tspdf_dict_get(item, "Next");
    }
}

TspdfError tspdf_reader_bookmarks(TspdfReader *doc, TspdfBookmarkInfo **out,
                                  size_t *count) {
    if (!doc || !out || !count) return TSPDF_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *catalog = bm_catalog(doc, &parser);
    if (!catalog) return TSPDF_OK;

    TspdfObj *root = tspdf_doctree_resolve(doc, &parser,
                                           tspdf_dict_get(catalog, "Outlines"));
    if (!root || root->type != TSPDF_OBJ_DICT) return TSPDF_OK;

    BmInfoList list = {0};
    BmNumList seen = {0};
    size_t budget = doc->xref.count + doc->new_objs.count + 16;
    bm_walk_level(doc, &parser, catalog, tspdf_dict_get(root, "First"),
                  1, 0, &budget, &seen, &list);
    free(seen.nums);
    if (list.oom) { free(list.items); return TSPDF_ERR_ALLOC; }

    if (list.count == 0) { free(list.items); return TSPDF_OK; }

    // Copy into the arena so the caller need not free (matches the form and
    // attachment enumerators).
    TspdfBookmarkInfo *arr = (TspdfBookmarkInfo *)tspdf_arena_alloc(
        &doc->arena, list.count * sizeof(TspdfBookmarkInfo));
    if (!arr) { free(list.items); return TSPDF_ERR_ALLOC; }
    memcpy(arr, list.items, list.count * sizeof(TspdfBookmarkInfo));
    free(list.items);
    *out = arr;
    *count = list.count;
    return TSPDF_OK;
}

// --- clear ---

// Drop a key from an arena-backed dict in place (compacting the entry array).
static void bm_dict_drop(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            for (size_t j = i + 1; j < dict->dict.count; j++) {
                dict->dict.entries[j - 1] = dict->dict.entries[j];
            }
            dict->dict.count--;
            return;
        }
    }
}

// Ensure the catalog is a mutable arena dict we can edit and that the trailer
// (and doc->catalog) point at it. Opened documents may have a catalog that is
// only reachable through the trailer /Root; we deep-copy it into the arena and
// register it so edits survive serialization.
static TspdfError bm_ensure_mutable_catalog(TspdfReader *doc, TspdfParser *parser,
                                            TspdfObj **out_catalog) {
    TspdfObj *catalog = bm_catalog(doc, parser);
    if (!catalog) return TSPDF_ERR_INVALID_ARG;

    if (doc->catalog == catalog) {
        *out_catalog = catalog;
        return TSPDF_OK;
    }
    // Reachable only through the trailer: copy it into the arena, register it,
    // and repoint the trailer /Root so the serializer writes the edited copy.
    TspdfObj *copy = tspdf_obj_deep_copy(catalog, &doc->arena);
    if (!copy) return TSPDF_ERR_ALLOC;
    uint32_t num = tspdf_register_new_obj(doc, copy);
    if (num == 0) return TSPDF_ERR_ALLOC;
    doc->catalog = copy;
    if (doc->xref.trailer) {
        TspdfObj *rref = (TspdfObj *)tspdf_arena_alloc_zero(&doc->arena,
                                                            sizeof(TspdfObj));
        if (!rref) return TSPDF_ERR_ALLOC;
        rref->type = TSPDF_OBJ_REF;
        rref->ref.num = num;
        TspdfError err = tspdf_obj_dict_put(doc->xref.trailer, "Root", rref,
                                            &doc->arena);
        if (err != TSPDF_OK) return err;
    }
    *out_catalog = copy;
    return TSPDF_OK;
}

TspdfError tspdf_reader_clear_bookmarks(TspdfReader *doc) {
    if (!doc) return TSPDF_ERR_INVALID_ARG;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    if (!bm_catalog(doc, &parser)) return TSPDF_ERR_INVALID_ARG;

    TspdfObj *catalog = NULL;
    TspdfError err = bm_ensure_mutable_catalog(doc, &parser, &catalog);
    if (err != TSPDF_OK) return err;

    bm_dict_drop(catalog, "Outlines");
    // /PageMode /UseOutlines would leave the viewer opening an empty panel.
    TspdfObj *pm = tspdf_dict_get(catalog, "PageMode");
    if (pm && pm->type == TSPDF_OBJ_NAME && pm->string.len == 12 &&
        memcmp(pm->string.data, "UseOutlines", 11) == 0) {
        bm_dict_drop(catalog, "PageMode");
    }
    doc->modified = true;
    return TSPDF_OK;
}

// --- set/replace ---

static TspdfObj *bm_obj_new(TspdfArena *a, TspdfObjType type) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = type;
    return o;
}

static TspdfObj *bm_obj_ref(TspdfArena *a, uint32_t num) {
    TspdfObj *o = bm_obj_new(a, TSPDF_OBJ_REF);
    if (o) o->ref.num = num;
    return o;
}

static TspdfObj *bm_obj_int(TspdfArena *a, int64_t v) {
    TspdfObj *o = bm_obj_new(a, TSPDF_OBJ_INT);
    if (o) o->integer = v;
    return o;
}

static TspdfObj *bm_obj_real(TspdfArena *a, double v) {
    TspdfObj *o = bm_obj_new(a, TSPDF_OBJ_REAL);
    if (o) o->real = v;
    return o;
}

static TspdfObj *bm_obj_null(TspdfArena *a) {
    return bm_obj_new(a, TSPDF_OBJ_NULL);
}

static TspdfObj *bm_obj_name(TspdfArena *a, const char *name) {
    TspdfObj *o = bm_obj_new(a, TSPDF_OBJ_NAME);
    if (!o) return NULL;
    size_t len = strlen(name);
    o->string.data = (uint8_t *)tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    memcpy(o->string.data, name, len + 1);
    o->string.len = len;
    return o;
}

// Build a /Title string object. ASCII titles become a plain string (the
// serializer escapes it); non-ASCII UTF-8 becomes UTF-16BE with a BOM so other
// readers do not misread the bytes.
static TspdfObj *bm_title_obj(TspdfArena *a, const char *title) {
    TspdfObj *o = bm_obj_new(a, TSPDF_OBJ_STRING);
    if (!o) return NULL;
    if (tspdf_str_is_ascii(title)) {
        size_t len = strlen(title);
        o->string.data = (uint8_t *)tspdf_arena_alloc(a, len ? len : 1);
        if (!o->string.data) return NULL;
        if (len) memcpy(o->string.data, title, len);
        o->string.len = len;
        return o;
    }
    // UTF-8 -> UTF-16BE with BOM (FE FF), matching the Info-string path.
    size_t out_cap = strlen(title) * 2 + 2;
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(a, out_cap ? out_cap : 2);
    if (!buf) return NULL;
    size_t n = 0;
    buf[n++] = 0xFE;
    buf[n++] = 0xFF;
    const char *p = title;
    while (*p) {
        uint32_t cp = 0;
        size_t used = tspdf_utf8_decode(p, &cp);
        if (used == 0) { p++; continue; }
        p += used;
        if (cp <= 0xFFFF) {
            buf[n++] = (uint8_t)(cp >> 8);
            buf[n++] = (uint8_t)(cp & 0xFF);
        } else {
            cp -= 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
            uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
            buf[n++] = (uint8_t)(hi >> 8);
            buf[n++] = (uint8_t)(hi & 0xFF);
            buf[n++] = (uint8_t)(lo >> 8);
            buf[n++] = (uint8_t)(lo & 0xFF);
        }
    }
    o->string.data = buf;
    o->string.len = n;
    return o;
}

// Build a /Dest array [<page-ref> /Fit] or [<page-ref> /XYZ 0 y null].
static TspdfObj *bm_dest_array(TspdfArena *a, uint32_t page_num,
                              bool has_y, double y) {
    TspdfObj *arr = bm_obj_new(a, TSPDF_OBJ_ARRAY);
    if (!arr) return NULL;
    size_t n = has_y ? 5 : 2;
    arr->array.items = (TspdfObj *)tspdf_arena_alloc(a, sizeof(TspdfObj) * n);
    if (!arr->array.items) return NULL;
    TspdfObj *pref = bm_obj_ref(a, page_num);
    if (!pref) return NULL;
    arr->array.items[0] = *pref;
    if (has_y) {
        TspdfObj *xyz = bm_obj_name(a, "XYZ");
        TspdfObj *zero = bm_obj_int(a, 0);
        TspdfObj *top = bm_obj_real(a, y);
        TspdfObj *zoom = bm_obj_null(a);
        if (!xyz || !zero || !top || !zoom) return NULL;
        arr->array.items[1] = *xyz;
        arr->array.items[2] = *zero;
        arr->array.items[3] = *top;
        arr->array.items[4] = *zoom;
    } else {
        TspdfObj *fit = bm_obj_name(a, "Fit");
        if (!fit) return NULL;
        arr->array.items[1] = *fit;
    }
    arr->array.count = n;
    return arr;
}

TspdfError tspdf_reader_set_bookmarks(TspdfReader *doc,
                                      const TspdfBookmarkEntry *entries,
                                      size_t count) {
    if (!doc) return TSPDF_ERR_INVALID_ARG;
    if (count > 0 && !entries) return TSPDF_ERR_INVALID_ARG;
    if (count == 0) return tspdf_reader_clear_bookmarks(doc);

    size_t npages = doc->pages.count;

    // Validate the flat list: levels start at 1, never jump up by >1, titles
    // are non-empty, pages are in range.
    int prev_level = 0;
    for (size_t i = 0; i < count; i++) {
        const TspdfBookmarkEntry *e = &entries[i];
        if (e->level < 1) return TSPDF_ERR_INVALID_ARG;
        if (i == 0 && e->level != 1) return TSPDF_ERR_INVALID_ARG;
        if (e->level > prev_level + 1) return TSPDF_ERR_INVALID_ARG;
        if (!e->title || e->title[0] == '\0') return TSPDF_ERR_INVALID_ARG;
        if (e->page_index >= npages) return TSPDF_ERR_PAGE_RANGE;
        prev_level = e->level;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *catalog = NULL;
    TspdfError err = bm_ensure_mutable_catalog(doc, &parser, &catalog);
    if (err != TSPDF_OK) return err;

    TspdfArena *a = &doc->arena;

    // Register one dict per entry, capturing each entry's object number so we
    // can wire /Parent, /Prev, /Next, /First, /Last, /Count by index.
    uint32_t *nums = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!nums) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < count; i++) {
        TspdfObj *item = bm_obj_new(a, TSPDF_OBJ_DICT);
        if (!item) { free(nums); return TSPDF_ERR_ALLOC; }
        nums[i] = tspdf_register_new_obj(doc, item);
        if (nums[i] == 0) { free(nums); return TSPDF_ERR_ALLOC; }
    }

    uint32_t root_num = 0;
    {
        TspdfObj *root = bm_obj_new(a, TSPDF_OBJ_DICT);
        if (!root) { free(nums); return TSPDF_ERR_ALLOC; }
        root_num = tspdf_register_new_obj(doc, root);
        if (root_num == 0) { free(nums); return TSPDF_ERR_ALLOC; }
    }

    // Levels above 127 would overflow the nesting stack; the validation loop
    // above accepts any positive level, so guard here.
    for (size_t i = 0; i < count; i++) {
        if (entries[i].level > 127) { free(nums); return TSPDF_ERR_INVALID_ARG; }
    }

    // parent_of[i] = object number of entry i's parent. stack[L] holds the
    // most recent item seen at level L; a level-L entry's parent is stack[L-1]
    // (or the root at level 1). Deeper stack slots are cleared when a
    // shallower entry appears so a stale deep item is never treated as parent.
    uint32_t *parent_of = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!parent_of) { free(nums); return TSPDF_ERR_ALLOC; }
    {
        uint32_t stack[128] = {0};
        for (size_t i = 0; i < count; i++) {
            int L = entries[i].level;
            parent_of[i] = (L == 1) ? root_num : stack[L - 1];
            if (parent_of[i] == 0) parent_of[i] = root_num;  // defensive
            stack[L] = nums[i];
            for (int k = L + 1; k < 128; k++) stack[k] = 0;
        }
    }

    // First pass: title, destination, and parent for every item.
    for (size_t i = 0; i < count && err == TSPDF_OK; i++) {
        const TspdfBookmarkEntry *e = &entries[i];
        uint32_t self = nums[i];
        TspdfObj *item = doc->new_objs.objs[self - doc->xref.count];

        TspdfObj *title = bm_title_obj(a, e->title);
        uint32_t page_num = (uint32_t)doc->pages.pages[e->page_index].obj_num;
        TspdfObj *dest = bm_dest_array(a, page_num, e->has_y, e->y);
        TspdfObj *pref = bm_obj_ref(a, parent_of[i]);
        if (!title || !dest || !pref) { err = TSPDF_ERR_ALLOC; break; }
        err = tspdf_obj_dict_put(item, "Title", title, a);
        if (err == TSPDF_OK) err = tspdf_obj_dict_put(item, "Dest", dest, a);
        if (err == TSPDF_OK) err = tspdf_obj_dict_put(item, "Parent", pref, a);
    }
    if (err != TSPDF_OK) { free(parent_of); free(nums); return err; }

    // Second pass: wire /Prev//Next (same-parent siblings) and
    // /First//Last//Count (children whose parent is this entry).
    for (size_t i = 0; i < count && err == TSPDF_OK; i++) {
        uint32_t self = nums[i];
        TspdfObj *item = doc->new_objs.objs[self - doc->xref.count];

        // Prev sibling: nearest earlier j with parent_of[j] == parent_of[i].
        for (size_t j = i; j-- > 0;) {
            if (parent_of[j] == parent_of[i]) {
                TspdfObj *ref = bm_obj_ref(a, nums[j]);
                if (!ref) { err = TSPDF_ERR_ALLOC; break; }
                err = tspdf_obj_dict_put(item, "Prev", ref, a);
                break;
            }
        }
        if (err != TSPDF_OK) break;
        // Next sibling: nearest later j with same parent.
        for (size_t j = i + 1; j < count; j++) {
            if (parent_of[j] == parent_of[i]) {
                TspdfObj *ref = bm_obj_ref(a, nums[j]);
                if (!ref) { err = TSPDF_ERR_ALLOC; break; }
                err = tspdf_obj_dict_put(item, "Next", ref, a);
                break;
            }
        }
        if (err != TSPDF_OK) break;

        // Children: first and last entries whose parent is `self`, plus the
        // count of visible descendants (all descendants, since every item we
        // write is open).
        size_t first_child = count, last_child = count;
        int64_t descendants = 0;
        for (size_t j = i + 1; j < count; j++) {
            if (entries[j].level <= entries[i].level) break;  // left the subtree
            descendants++;
            if (parent_of[j] == self) {
                if (first_child == count) first_child = j;
                last_child = j;
            }
        }
        if (first_child != count) {
            TspdfObj *fref = bm_obj_ref(a, nums[first_child]);
            TspdfObj *lref = bm_obj_ref(a, nums[last_child]);
            TspdfObj *cnt = bm_obj_int(a, descendants);
            if (!fref || !lref || !cnt) { err = TSPDF_ERR_ALLOC; break; }
            err = tspdf_obj_dict_put(item, "First", fref, a);
            if (err == TSPDF_OK) err = tspdf_obj_dict_put(item, "Last", lref, a);
            if (err == TSPDF_OK) err = tspdf_obj_dict_put(item, "Count", cnt, a);
        }
    }

    free(parent_of);
    if (err != TSPDF_OK) { free(nums); return err; }

    // Build the /Outlines root over the top-level items.
    size_t top_first = count, top_last = count;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].level == 1) {
            if (top_first == count) top_first = i;
            top_last = i;
        }
    }
    TspdfObj *root = doc->new_objs.objs[root_num - doc->xref.count];
    TspdfObj *type = bm_obj_name(a, "Outlines");
    TspdfObj *fref = bm_obj_ref(a, nums[top_first]);
    TspdfObj *lref = bm_obj_ref(a, nums[top_last]);
    TspdfObj *cnt = bm_obj_int(a, (int64_t)count);  // all items open/visible
    if (!type || !fref || !lref || !cnt) { free(nums); return TSPDF_ERR_ALLOC; }
    err = tspdf_obj_dict_put(root, "Type", type, a);
    if (err == TSPDF_OK) err = tspdf_obj_dict_put(root, "First", fref, a);
    if (err == TSPDF_OK) err = tspdf_obj_dict_put(root, "Last", lref, a);
    if (err == TSPDF_OK) err = tspdf_obj_dict_put(root, "Count", cnt, a);
    free(nums);
    if (err != TSPDF_OK) return err;

    // Point the catalog at the new root.
    TspdfObj *rref = bm_obj_ref(a, root_num);
    if (!rref) return TSPDF_ERR_ALLOC;
    err = tspdf_obj_dict_put(catalog, "Outlines", rref, a);
    if (err != TSPDF_OK) return err;

    doc->modified = true;
    return TSPDF_OK;
}
