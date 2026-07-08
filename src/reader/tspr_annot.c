#include "tspr_internal.h"
#include <stdlib.h>
#include <string.h>

// --- TspdfArena-allocated object helpers ---

static TspdfObj *annot_make_name(TspdfArena *a, const char *str) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_NAME;
    size_t len = strlen(str);
    obj->string.data = tspdf_arena_alloc(a, len + 1);
    memcpy(obj->string.data, str, len + 1);
    obj->string.len = len;
    return obj;
}

static TspdfObj *annot_make_string(TspdfArena *a, const char *str) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_STRING;
    size_t len = strlen(str);
    obj->string.data = tspdf_arena_alloc(a, len + 1);
    memcpy(obj->string.data, str, len + 1);
    obj->string.len = len;
    return obj;
}

static TspdfObj *annot_make_bool(TspdfArena *a, bool val) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_BOOL;
    obj->boolean = val;
    return obj;
}

static TspdfObj *annot_make_ref(TspdfArena *a, uint32_t num) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_REF;
    obj->ref.num = num;
    obj->ref.gen = 0;
    return obj;
}

static TspdfObj *annot_make_dict(TspdfArena *a, size_t cap) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_DICT;
    obj->dict.entries = tspdf_arena_alloc_zero(a, cap * sizeof(TspdfDictEntry));
    return obj;
}

static void annot_dict_add(TspdfObj *d, TspdfArena *a, const char *key, TspdfObj *val) {
    size_t idx = d->dict.count++;
    d->dict.entries[idx].key = tspdf_arena_alloc(a, strlen(key) + 1);
    memcpy(d->dict.entries[idx].key, key, strlen(key) + 1);
    d->dict.entries[idx].value = val;
}

static TspdfObj *annot_make_array(TspdfArena *a, size_t cap) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    obj->type = TSPDF_OBJ_ARRAY;
    obj->array.items = tspdf_arena_alloc_zero(a, cap * sizeof(TspdfObj));
    return obj;
}

static TspdfObj *annot_make_rect(TspdfArena *a, double x, double y, double w, double h) {
    TspdfObj *arr = annot_make_array(a, 4);
    arr->array.items[0] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = x};
    arr->array.items[1] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = y};
    arr->array.items[2] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = x + w};
    arr->array.items[3] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = y + h};
    arr->array.count = 4;
    return arr;
}

// --- Helper: add annotation to page ---

static TspdfError add_annot_to_page(TspdfReader *doc, size_t page_index, TspdfObj *annot) {
    if (page_index >= doc->pages.count) return TSPDF_ERR_INVALID_PDF;
    TspdfReaderPage *page = &doc->pages.pages[page_index];
    TspdfArena *a = &doc->arena;

    // Register as indirect object
    uint32_t annot_num = tspdf_register_new_obj(doc, annot);
    if (annot_num == 0) return TSPDF_ERR_ALLOC;

    // Get or create /Annots array on page dict
    TspdfObj *annots = tspdf_dict_get(page->page_dict, "Annots");
    if (!annots || annots->type != TSPDF_OBJ_ARRAY) {
        // Create new /Annots array and add to page dict
        annots = annot_make_array(a, 16);
        // Add /Annots to page dict — need to rebuild entries array
        size_t old_count = page->page_dict->dict.count;
        TspdfDictEntry *new_entries = tspdf_arena_alloc(a, (old_count + 1) * sizeof(TspdfDictEntry));
        memcpy(new_entries, page->page_dict->dict.entries, old_count * sizeof(TspdfDictEntry));
        new_entries[old_count].key = tspdf_arena_alloc(a, 7);
        memcpy(new_entries[old_count].key, "Annots", 7);
        new_entries[old_count].value = annots;
        page->page_dict->dict.entries = new_entries;
        page->page_dict->dict.count = old_count + 1;
    }

    // Add ref to annotation in /Annots array
    TspdfObj *ref = annot_make_ref(a, annot_num);
    // Need to grow array — rebuild items
    size_t old_arr_count = annots->array.count;
    TspdfObj *new_items = tspdf_arena_alloc(a, (old_arr_count + 1) * sizeof(TspdfObj));
    if (old_arr_count > 0)
        memcpy(new_items, annots->array.items, old_arr_count * sizeof(TspdfObj));
    new_items[old_arr_count] = *ref;
    annots->array.items = new_items;
    annots->array.count = old_arr_count + 1;

    doc->modified = true;
    return TSPDF_OK;
}

// --- Annotation implementations ---

TspdfError tspdf_page_add_link(TspdfReader *doc, size_t page_index,
                              double x, double y, double w, double h, const char *url) {
    if (!doc || !url) return TSPDF_ERR_INVALID_PDF;
    TspdfArena *a = &doc->arena;

    TspdfObj *annot = annot_make_dict(a, 8);
    annot_dict_add(annot, a, "Type", annot_make_name(a, "Annot"));
    annot_dict_add(annot, a, "Subtype", annot_make_name(a, "Link"));
    annot_dict_add(annot, a, "Rect", annot_make_rect(a, x, y, w, h));

    // /Border [0 0 0] (no visible border)
    TspdfObj *border = annot_make_array(a, 3);
    border->array.items[0] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.items[1] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.items[2] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.count = 3;
    annot_dict_add(annot, a, "Border", border);

    // /A << /Type /Action /S /URI /URI (url) >>
    TspdfObj *action = annot_make_dict(a, 4);
    annot_dict_add(action, a, "Type", annot_make_name(a, "Action"));
    annot_dict_add(action, a, "S", annot_make_name(a, "URI"));
    annot_dict_add(action, a, "URI", annot_make_string(a, url));
    annot_dict_add(annot, a, "A", action);

    return add_annot_to_page(doc, page_index, annot);
}

TspdfError tspdf_page_add_link_to_page(TspdfReader *doc, size_t page_index,
                                      double x, double y, double w, double h, size_t target_page) {
    if (!doc || target_page >= doc->pages.count) return TSPDF_ERR_INVALID_PDF;
    TspdfArena *a = &doc->arena;

    TspdfObj *annot = annot_make_dict(a, 6);
    annot_dict_add(annot, a, "Type", annot_make_name(a, "Annot"));
    annot_dict_add(annot, a, "Subtype", annot_make_name(a, "Link"));
    annot_dict_add(annot, a, "Rect", annot_make_rect(a, x, y, w, h));

    TspdfObj *border = annot_make_array(a, 3);
    border->array.items[0] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.items[1] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.items[2] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    border->array.count = 3;
    annot_dict_add(annot, a, "Border", border);

    // /Dest [page_ref /Fit] — MUST use TSPDF_OBJ_REF for renumbering
    TspdfObj *dest = annot_make_array(a, 2);
    uint32_t page_obj_num = doc->pages.pages[target_page].obj_num;
    dest->array.items[0] = (TspdfObj){.type = TSPDF_OBJ_REF, .ref = {.num = page_obj_num, .gen = 0}};
    dest->array.items[1] = (TspdfObj){.type = TSPDF_OBJ_NAME};
    dest->array.items[1].string.data = tspdf_arena_alloc(a, 4);
    memcpy(dest->array.items[1].string.data, "Fit", 4);
    dest->array.items[1].string.len = 3;
    dest->array.count = 2;
    annot_dict_add(annot, a, "Dest", dest);

    return add_annot_to_page(doc, page_index, annot);
}

TspdfError tspdf_page_add_text_note(TspdfReader *doc, size_t page_index,
                                   double x, double y, const char *title, const char *contents) {
    if (!doc) return TSPDF_ERR_INVALID_PDF;
    TspdfArena *a = &doc->arena;

    TspdfObj *annot = annot_make_dict(a, 8);
    annot_dict_add(annot, a, "Type", annot_make_name(a, "Annot"));
    annot_dict_add(annot, a, "Subtype", annot_make_name(a, "Text"));
    annot_dict_add(annot, a, "Rect", annot_make_rect(a, x, y, 24, 24));
    if (title) annot_dict_add(annot, a, "T", annot_make_string(a, title));
    if (contents) annot_dict_add(annot, a, "Contents", annot_make_string(a, contents));
    annot_dict_add(annot, a, "Open", annot_make_bool(a, false));

    // /C [1 1 0] (yellow)
    TspdfObj *color = annot_make_array(a, 3);
    color->array.items[0] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 1};
    color->array.items[1] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 1};
    color->array.items[2] = (TspdfObj){.type = TSPDF_OBJ_INT, .integer = 0};
    color->array.count = 3;
    annot_dict_add(annot, a, "C", color);

    return add_annot_to_page(doc, page_index, annot);
}

TspdfError tspdf_page_add_stamp(TspdfReader *doc, size_t page_index,
                               double x, double y, double w, double h, const char *stamp_name) {
    if (!doc || !stamp_name) return TSPDF_ERR_INVALID_PDF;
    TspdfArena *a = &doc->arena;

    TspdfObj *annot = annot_make_dict(a, 6);
    annot_dict_add(annot, a, "Type", annot_make_name(a, "Annot"));
    annot_dict_add(annot, a, "Subtype", annot_make_name(a, "Stamp"));
    annot_dict_add(annot, a, "Rect", annot_make_rect(a, x, y, w, h));
    annot_dict_add(annot, a, "Name", annot_make_name(a, stamp_name));

    return add_annot_to_page(doc, page_index, annot);
}
