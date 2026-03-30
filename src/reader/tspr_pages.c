#include "tspr_internal.h"
#include <string.h>
#include <stdlib.h>

// Parse a /MediaBox array [x0 y0 x1 y1] into 4 doubles.
// Returns true on success.
static bool parse_media_box(TspdfObj *obj, double out[4]) {
    if (!obj || obj->type != TSPDF_OBJ_ARRAY || obj->array.count < 4)
        return false;
    for (int i = 0; i < 4; i++) {
        TspdfObj *item = &obj->array.items[i];
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

// Temporary growable page list for building during tree walk
typedef struct {
    TspdfReaderPage *pages;
    size_t count;
    size_t capacity;
} PageCollector;

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

// Walk a page tree node recursively.
// inherited_media_box: pointer to 4 doubles or NULL if not inherited yet.
// inherited_rotate: the inherited rotation value (-1 if not set).
static TspdfError walk_page_tree(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                                TspdfObj *node, uint32_t node_obj_num,
                                PageCollector *collector,
                                const double *inherited_media_box, int inherited_rotate,
                                TspdfCrypt *crypt) {
    if (!node || node->type != TSPDF_OBJ_DICT)
        return TSPDF_ERR_PARSE;

    TspdfObj *type_obj = tspdf_dict_get(node, "Type");
    if (!type_obj || type_obj->type != TSPDF_OBJ_NAME)
        return TSPDF_ERR_PARSE;

    const char *type_name = (const char *)type_obj->string.data;

    // Determine current node's MediaBox and Rotate (for inheritance)
    double current_media_box[4] = {0};
    bool has_media_box = false;
    TspdfObj *mb = tspdf_dict_get(node, "MediaBox");
    if (mb && parse_media_box(mb, current_media_box)) {
        has_media_box = true;
    } else if (inherited_media_box) {
        memcpy(current_media_box, inherited_media_box, sizeof(double) * 4);
        has_media_box = true;
    }

    int current_rotate = inherited_rotate;
    TspdfObj *rot = tspdf_dict_get(node, "Rotate");
    if (rot && rot->type == TSPDF_OBJ_INT) {
        current_rotate = (int)rot->integer;
    }

    if (strcmp(type_name, "Pages") == 0) {
        // Intermediate node: recurse into /Kids
        TspdfObj *kids = tspdf_dict_get(node, "Kids");
        if (!kids || kids->type != TSPDF_OBJ_ARRAY)
            return TSPDF_ERR_PARSE;

        for (size_t i = 0; i < kids->array.count; i++) {
            TspdfObj *kid_ref = &kids->array.items[i];
            if (kid_ref->type != TSPDF_OBJ_REF)
                return TSPDF_ERR_PARSE;

            TspdfObj *kid = tspdf_xref_resolve(xref, p, kid_ref->ref.num, cache, crypt);
            if (!kid)
                return TSPDF_ERR_PARSE;

            TspdfError err = walk_page_tree(p, xref, cache, kid, kid_ref->ref.num,
                                           collector,
                                           has_media_box ? current_media_box : NULL,
                                           current_rotate, crypt);
            if (err != TSPDF_OK) return err;
        }
    } else if (strcmp(type_name, "Page") == 0) {
        // Leaf node: create TspdfReaderPage
        TspdfReaderPage page = {0};
        page.obj_num = node_obj_num;
        page.page_dict = node;

        if (has_media_box) {
            memcpy(page.media_box, current_media_box, sizeof(double) * 4);
        }

        page.rotate = (current_rotate >= 0) ? current_rotate : 0;

        if (!collector_push(collector, &page))
            return TSPDF_ERR_ALLOC;
    } else {
        return TSPDF_ERR_PARSE;
    }

    return TSPDF_OK;
}

TspdfError tspdf_pages_load(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                          TspdfObj *catalog, TspdfReaderPageList *out, TspdfCrypt *crypt) {
    // Get /Pages ref from catalog
    TspdfObj *pages_ref = tspdf_dict_get(catalog, "Pages");
    if (!pages_ref || pages_ref->type != TSPDF_OBJ_REF)
        return TSPDF_ERR_PARSE;

    TspdfObj *pages_obj = tspdf_xref_resolve(xref, p, pages_ref->ref.num, cache, crypt);
    if (!pages_obj)
        return TSPDF_ERR_PARSE;

    // Walk the tree, collecting pages into a temp malloc'd array
    PageCollector collector = {0};
    TspdfError err = walk_page_tree(p, xref, cache, pages_obj, pages_ref->ref.num, &collector, NULL, -1, crypt);
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
