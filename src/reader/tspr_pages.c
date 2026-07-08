#include "tspr_internal.h"
#include <string.h>
#include <stdlib.h>

// Temporary growable page list for building during tree walk
typedef struct {
    TspdfReaderPage *pages;
    size_t count;
    size_t capacity;
} PageCollector;

typedef struct {
    bool *visiting;
    size_t count;
    size_t max_depth;
} PageWalkGuard;

typedef enum {
    PAGE_NODE_INVALID = 0,
    PAGE_NODE_PAGES,
    PAGE_NODE_PAGE
} PageNodeKind;

static int normalize_rotation(int rotate) {
    if (rotate % 90 != 0) {
        return 0;
    }

    int normalized = rotate % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    return normalized;
}

static bool collector_push(PageCollector *c, TspdfReaderPage *page) {
    if (c->count >= c->capacity) {
        size_t new_cap = c->capacity == 0 ? 16 : c->capacity * 2;
        TspdfReaderPage *new_pages = (TspdfReaderPage *)realloc(c->pages, sizeof(TspdfReaderPage) * new_cap);
        if (!new_pages) return false;
        c->pages = new_pages;
        c->capacity = new_cap;
    }
    c->pages[c->count++] = *page;
    return true;
}

static TspdfObj *resolve_if_ref(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                                TspdfObj *obj, TspdfCrypt *crypt) {
    if (!obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }

    return tspdf_xref_resolve(xref, p, obj->ref.num, cache, crypt);
}

static PageNodeKind classify_page_node(TspdfParser *p, TspdfReaderXref *xref,
                                       TspdfObj **cache, TspdfObj *node,
                                       TspdfCrypt *crypt, TspdfObj **out_kids) {
    if (out_kids) {
        *out_kids = NULL;
    }

    TspdfObj *type_ref = tspdf_dict_get(node, "Type");
    if (type_ref) {
        TspdfObj *type_obj = resolve_if_ref(p, xref, cache, type_ref, crypt);
        if (!type_obj || type_obj->type != TSPDF_OBJ_NAME) {
            return PAGE_NODE_INVALID;
        }

        const char *type_name = (const char *)type_obj->string.data;
        if (strcmp(type_name, "Pages") == 0) {
            return PAGE_NODE_PAGES;
        }
        if (strcmp(type_name, "Page") == 0) {
            return PAGE_NODE_PAGE;
        }
        return PAGE_NODE_INVALID;
    }

    TspdfObj *kids = resolve_if_ref(p, xref, cache, tspdf_dict_get(node, "Kids"), crypt);
    if (kids && kids->type == TSPDF_OBJ_ARRAY) {
        if (out_kids) {
            *out_kids = kids;
        }
        return PAGE_NODE_PAGES;
    }

    return PAGE_NODE_PAGE;
}

// Parse a page box array [x0 y0 x1 y1] into 4 doubles.
// Some producer output stores individual scalar values as indirect objects.
static bool parse_page_box(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                           TspdfObj *obj, double out[4], TspdfCrypt *crypt) {
    if (!obj || obj->type != TSPDF_OBJ_ARRAY || obj->array.count < 4)
        return false;
    for (int i = 0; i < 4; i++) {
        TspdfObj *item = resolve_if_ref(p, xref, cache, &obj->array.items[i], crypt);
        if (!item)
            return false;
        if (item->type == TSPDF_OBJ_INT) {
            out[i] = (double)item->integer;
        } else if (item->type == TSPDF_OBJ_REAL) {
            out[i] = item->real;
        } else {
            return false;
        }
    }
    return true;
}

static bool parse_positive_number_obj(TspdfParser *p, TspdfReaderXref *xref,
                                      TspdfObj **cache, TspdfObj *obj,
                                      TspdfCrypt *crypt, double *out) {
    TspdfObj *resolved = resolve_if_ref(p, xref, cache, obj, crypt);
    if (!resolved || !out) {
        return false;
    }

    double value = 0.0;
    if (resolved->type == TSPDF_OBJ_INT) {
        value = (double)resolved->integer;
    } else if (resolved->type == TSPDF_OBJ_REAL) {
        value = resolved->real;
    } else {
        return false;
    }

    if (value <= 0.0) {
        return false;
    }

    *out = value;
    return true;
}

// Walk a page tree node recursively.
// inherited_media_box/inherited_crop_box: pointers to 4 doubles or NULL if not inherited yet.
// inherited_rotate: the inherited rotation value (-1 if not set).
static TspdfError walk_page_tree(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                                TspdfObj *node, uint32_t node_obj_num,
                                PageCollector *collector,
                                const double *inherited_media_box,
                                const double *inherited_crop_box,
                                int inherited_rotate,
                                double inherited_user_unit,
                                TspdfCrypt *crypt, PageWalkGuard *guard, size_t depth) {
    if (guard && depth > guard->max_depth)
        return TSPDF_ERR_PARSE;

    if (!node || node->type != TSPDF_OBJ_DICT)
        return TSPDF_ERR_PARSE;

    bool marked_visiting = false;
    if (guard && node_obj_num > 0 && node_obj_num < guard->count) {
        if (guard->visiting[node_obj_num])
            return TSPDF_ERR_PARSE;
        guard->visiting[node_obj_num] = true;
        marked_visiting = true;
    }

    TspdfObj *kids = NULL;
    PageNodeKind node_kind = classify_page_node(p, xref, cache, node, crypt, &kids);
    if (node_kind == PAGE_NODE_INVALID) {
        if (marked_visiting) guard->visiting[node_obj_num] = false;
        return TSPDF_ERR_PARSE;
    }

    // Determine current node's MediaBox, CropBox, and Rotate (for inheritance)
    double current_media_box[4] = {0};
    bool has_media_box = false;
    TspdfObj *mb = resolve_if_ref(p, xref, cache, tspdf_dict_get(node, "MediaBox"), crypt);
    if (mb && parse_page_box(p, xref, cache, mb, current_media_box, crypt)) {
        has_media_box = true;
    } else if (inherited_media_box) {
        memcpy(current_media_box, inherited_media_box, sizeof(double) * 4);
        has_media_box = true;
    }

    double current_crop_box[4] = {0};
    bool has_crop_box = false;
    TspdfObj *cb = resolve_if_ref(p, xref, cache, tspdf_dict_get(node, "CropBox"), crypt);
    if (cb && parse_page_box(p, xref, cache, cb, current_crop_box, crypt)) {
        has_crop_box = true;
    } else if (inherited_crop_box) {
        memcpy(current_crop_box, inherited_crop_box, sizeof(double) * 4);
        has_crop_box = true;
    }

    int current_rotate = inherited_rotate;
    TspdfObj *rot = resolve_if_ref(p, xref, cache, tspdf_dict_get(node, "Rotate"), crypt);
    if (rot && rot->type == TSPDF_OBJ_INT) {
        current_rotate = (int)rot->integer;
    }

    double current_user_unit = inherited_user_unit > 0.0 ? inherited_user_unit : 1.0;
    double parsed_user_unit = 0.0;
    if (parse_positive_number_obj(p, xref, cache, tspdf_dict_get(node, "UserUnit"),
                                  crypt, &parsed_user_unit)) {
        current_user_unit = parsed_user_unit;
    }

    if (node_kind == PAGE_NODE_PAGES) {
        // Intermediate node: recurse into /Kids
        if (!kids) {
            kids = resolve_if_ref(p, xref, cache, tspdf_dict_get(node, "Kids"), crypt);
        }
        if (!kids || kids->type != TSPDF_OBJ_ARRAY) {
            if (marked_visiting) guard->visiting[node_obj_num] = false;
            return TSPDF_ERR_PARSE;
        }

        for (size_t i = 0; i < kids->array.count; i++) {
            TspdfObj *kid_ref = &kids->array.items[i];
            TspdfObj *kid = resolve_if_ref(p, xref, cache, kid_ref, crypt);
            if (!kid) {
                if (marked_visiting) guard->visiting[node_obj_num] = false;
                return TSPDF_ERR_PARSE;
            }

            uint32_t kid_obj_num = kid_ref->type == TSPDF_OBJ_REF ? kid_ref->ref.num : 0;
            TspdfError err = walk_page_tree(p, xref, cache, kid, kid_obj_num,
                                           collector,
                                           has_media_box ? current_media_box : NULL,
                                           has_crop_box ? current_crop_box : NULL,
                                           current_rotate, current_user_unit,
                                           crypt, guard, depth + 1);
            if (err != TSPDF_OK) {
                if (marked_visiting) guard->visiting[node_obj_num] = false;
                return err;
            }
        }
    } else if (node_kind == PAGE_NODE_PAGE) {
        // Leaf node: create TspdfReaderPage
        TspdfReaderPage page = {0};
        page.obj_num = node_obj_num;
        page.page_dict = node;

        if (has_media_box) {
            memcpy(page.media_box, current_media_box, sizeof(double) * 4);
        } else if (has_crop_box) {
            memcpy(page.media_box, current_crop_box, sizeof(double) * 4);
        }

        page.rotate = current_rotate >= 0 ? normalize_rotation(current_rotate) :
                      (current_rotate < -1 ? normalize_rotation(current_rotate) : 0);
        page.user_unit = current_user_unit > 0.0 ? current_user_unit : 1.0;

        if (!collector_push(collector, &page)) {
            if (marked_visiting) guard->visiting[node_obj_num] = false;
            return TSPDF_ERR_ALLOC;
        }
    }

    if (marked_visiting) {
        guard->visiting[node_obj_num] = false;
    }
    return TSPDF_OK;
}

TspdfError tspdf_pages_load(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                          TspdfObj *catalog, TspdfReaderPageList *out, TspdfCrypt *crypt) {
    // Get /Pages ref from catalog
    TspdfObj *pages_ref = tspdf_dict_get(catalog, "Pages");
    TspdfObj *pages_obj = resolve_if_ref(p, xref, cache, pages_ref, crypt);
    if (!pages_obj || pages_obj->type != TSPDF_OBJ_DICT)
        return TSPDF_ERR_PARSE;

    uint32_t pages_obj_num = pages_ref && pages_ref->type == TSPDF_OBJ_REF ? pages_ref->ref.num : 0;

    PageWalkGuard guard = {0};
    guard.count = xref ? xref->count : 0;
    guard.max_depth = guard.count > 0 ? guard.count + 32 : 1024;
    if (guard.max_depth < 1024) guard.max_depth = 1024;
    if (guard.count > 0) {
        guard.visiting = (bool *)calloc(guard.count, sizeof(bool));
        if (!guard.visiting)
            return TSPDF_ERR_ALLOC;
    }

    // Walk the tree, collecting pages into a temp malloc'd array
    PageCollector collector = {0};
    TspdfError err = walk_page_tree(p, xref, cache, pages_obj, pages_obj_num,
                                    &collector, NULL, NULL, -1, 1.0,
                                    crypt, &guard, 0);
    free(guard.visiting);
    if (err != TSPDF_OK) {
        free(collector.pages);
        return err;
    }

    // Copy pages into the arena
    if (collector.count > 0) {
        TspdfReaderPage *arena_pages = (TspdfReaderPage *)tspdf_arena_alloc(p->arena,
                                    sizeof(TspdfReaderPage) * collector.count);
        if (!arena_pages) {
            free(collector.pages);
            return TSPDF_ERR_ALLOC;
        }
        memcpy(arena_pages, collector.pages, sizeof(TspdfReaderPage) * collector.count);
        out->pages = arena_pages;
    } else {
        out->pages = NULL;
    }
    out->count = collector.count;

    free(collector.pages);
    return TSPDF_OK;
}
