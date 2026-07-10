#include "tspr_internal.h"
#include "../pdf/tspdf_writer.h"
#include "../pdf/pdf_stream.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// --- Helper functions for creating TspdfObj nodes ---

static TspdfObj *make_name_obj(TspdfArena *a, const char *str) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_NAME;
    size_t slen = strlen(str);
    obj->string.data = tspdf_arena_alloc(a, slen + 1);
    memcpy(obj->string.data, str, slen + 1);
    obj->string.len = slen;
    return obj;
}

static TspdfObj *make_int_obj(TspdfArena *a, int64_t val) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_INT;
    obj->integer = val;
    return obj;
}

static TspdfObj *make_ref_obj(TspdfArena *a, uint32_t num) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_REF;
    obj->ref.num = num;
    obj->ref.gen = 0;
    return obj;
}

static TspdfObj *make_dict_obj(TspdfArena *a, size_t capacity) {
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_DICT;
    obj->dict.entries = tspdf_arena_alloc_zero(a, capacity * sizeof(TspdfDictEntry));
    obj->dict.count = 0;
    return obj;
}

static void dict_add(TspdfObj *dict, TspdfArena *a, const char *key, TspdfObj *value) {
    size_t idx = dict->dict.count++;
    size_t klen = strlen(key);
    dict->dict.entries[idx].key = tspdf_arena_alloc(a, klen + 1);
    memcpy(dict->dict.entries[idx].key, key, klen + 1);
    dict->dict.entries[idx].value = value;
}

static TspdfObj *make_stream_obj(TspdfArena *a, const uint8_t *data, size_t len) {
    // Create stream dict
    TspdfObj *sdict = make_dict_obj(a, 4);
    if (!sdict) return NULL;
    dict_add(sdict, a, "Length", make_int_obj(a, (int64_t)len));

    // Create stream object
    TspdfObj *obj = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_STREAM;
    obj->stream.dict = sdict;
    obj->stream.data = tspdf_arena_alloc(a, len);
    memcpy(obj->stream.data, data, len);
    obj->stream.len = len;
    obj->stream.self_contained = true;
    return obj;
}

// --- Build new resources dict from TspdfWriter ---

static TspdfObj *build_new_resources(TspdfWriter *resource_owner, TspdfArena *arena) {
    // Count how many resource categories we need
    int has_fonts = (resource_owner->font_count > 0);
    int has_images = (resource_owner->image_count > 0);
    int has_extgstate = (resource_owner->opacity_count > 0);
    int cat_count = has_fonts + has_images + has_extgstate;
    if (cat_count == 0) return NULL;

    TspdfObj *resources = make_dict_obj(arena, (size_t)(cat_count + 1));
    if (!resources) return NULL;

    // Build /Font sub-dict
    if (has_fonts) {
        TspdfObj *font_dict = make_dict_obj(arena, (size_t)resource_owner->font_count);
        if (!font_dict) return NULL;

        for (int i = 0; i < resource_owner->font_count; i++) {
            TspdfFont *f = &resource_owner->fonts[i];

            if (f->is_builtin) {
                // << /Type /Font /Subtype /Type1 /BaseFont /name /Encoding /WinAnsiEncoding >>
                TspdfObj *fobj = make_dict_obj(arena, 4);
                if (!fobj) return NULL;
                dict_add(fobj, arena, "Type", make_name_obj(arena, "Font"));
                dict_add(fobj, arena, "Subtype", make_name_obj(arena, "Type1"));
                dict_add(fobj, arena, "BaseFont", make_name_obj(arena, f->base_font_name));
                dict_add(fobj, arena, "Encoding", make_name_obj(arena, "WinAnsiEncoding"));

                dict_add(font_dict, arena, f->name, fobj);
            }
            // TTF fonts deferred for Phase 3 MVP
        }

        if (font_dict->dict.count > 0) {
            dict_add(resources, arena, "Font", font_dict);
        }
    }

    // Build /ExtGState sub-dict
    if (has_extgstate) {
        TspdfObj *gs_dict = make_dict_obj(arena, (size_t)resource_owner->opacity_count);
        if (!gs_dict) return NULL;

        for (int i = 0; i < resource_owner->opacity_count; i++) {
            TspdfOpacityState *gs = &resource_owner->opacity_states[i];
            TspdfObj *gsobj = make_dict_obj(arena, 3);
            if (!gsobj) return NULL;
            dict_add(gsobj, arena, "Type", make_name_obj(arena, "ExtGState"));
            // ca = fill opacity, CA = stroke opacity
            // Use integer representation for int-valued opacities
            TspdfObj *fill_val = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
            fill_val->type = TSPDF_OBJ_REAL;
            fill_val->real = gs->fill_opacity;
            TspdfObj *stroke_val = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
            stroke_val->type = TSPDF_OBJ_REAL;
            stroke_val->real = gs->stroke_opacity;
            dict_add(gsobj, arena, "ca", fill_val);
            dict_add(gsobj, arena, "CA", stroke_val);

            dict_add(gs_dict, arena, gs->name, gsobj);
        }

        if (gs_dict->dict.count > 0) {
            dict_add(resources, arena, "ExtGState", gs_dict);
        }
    }

    // Images as XObjects - Phase 3 MVP: skip image embedding (complex, needs stream objects)
    // For now, only fonts and ExtGState are supported.

    return resources;
}

// --- Public API ---

TspdfStream *tspdf_page_begin_content(TspdfReader *doc, size_t page_index) {
    if (!doc || page_index >= doc->pages.count) return NULL;
    TspdfStream *stream = malloc(sizeof(TspdfStream));
    if (!stream) return NULL;
    *stream = tspdf_stream_create();
    return stream;
}

static TspdfError page_end_content_internal(TspdfReader *doc, size_t page_index,
                                            TspdfStream *stream, TspdfWriter *resource_owner,
                                            bool under) {
    if (!doc || !stream) return TSPDF_ERR_INVALID_PDF;
    if (page_index >= doc->pages.count) return TSPDF_ERR_INVALID_PDF;

    TspdfArena *arena = &doc->arena;
    TspdfReaderPage *page = &doc->pages.pages[page_index];
    TspdfObj *page_dict = page->page_dict;

    // 1. Get raw content bytes from the TspdfStream
    const uint8_t *content_data = stream->buf.data;
    size_t content_len = stream->buf.len;

    if (content_len == 0) {
        // Nothing to overlay
        tspdf_stream_destroy(stream);
        free(stream);
        return TSPDF_OK;
    }

    // 2. Build new resources from resource_owner
    TspdfObj *new_resources = NULL;
    if (resource_owner) {
        new_resources = build_new_resources(resource_owner, arena);
    }

    // 3. Merge resources
    TspdfRenameMap renames = {0};
    if (new_resources) {
        TspdfError err = tspdf_resources_merge(page_dict, new_resources, arena, &renames);
        if (err != TSPDF_OK) {
            tspdf_stream_destroy(stream);
            free(stream);
            return err;
        }
    }

    // 4. Rewrite content stream if renames occurred
    const uint8_t *final_content = content_data;
    size_t final_len = content_len;
    if (renames.count > 0) {
        size_t rewritten_len = 0;
        uint8_t *rewritten = tspdf_content_rewrite(content_data, content_len,
                                                   &renames, &rewritten_len, arena);
        if (rewritten) {
            final_content = rewritten;
            final_len = rewritten_len;
        }
    }

    // 5-8. Build the new stream object(s) and the replacement /Contents array.
    TspdfObj *contents = tspdf_dict_get(page_dict, "Contents");

    TspdfObj *new_contents_array = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
    if (!new_contents_array) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
    new_contents_array->type = TSPDF_OBJ_ARRAY;

    if (under) {
        // Under-mode: prepend one stream wrapped in q..Q, so the original
        // content still starts from a pristine graphics state:
        // [wrapped_new, ...old items]
        size_t wrapped_len = 2 + final_len + 3;  // "q\n" + content + "\nQ\n"
        uint8_t *wrapped = tspdf_arena_alloc(arena, wrapped_len);
        if (!wrapped) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
        memcpy(wrapped, "q\n", 2);
        memcpy(wrapped + 2, final_content, final_len);
        memcpy(wrapped + 2 + final_len, "\nQ\n", 3);

        TspdfObj *new_stream = make_stream_obj(arena, wrapped, wrapped_len);
        if (!new_stream) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
        uint32_t new_num = tspdf_register_new_obj(doc, new_stream);
        if (new_num == 0) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
        TspdfObj *new_ref = make_ref_obj(arena, new_num);
        if (!new_ref) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }

        size_t old_count = 0;
        if (contents && contents->type == TSPDF_OBJ_ARRAY) old_count = contents->array.count;
        else if (contents && contents->type == TSPDF_OBJ_REF) old_count = 1;

        new_contents_array->array.items = tspdf_arena_alloc_zero(arena, (old_count + 1) * sizeof(TspdfObj));
        if (!new_contents_array->array.items) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
        new_contents_array->array.count = old_count + 1;
        new_contents_array->array.items[0] = *new_ref;
        if (contents && contents->type == TSPDF_OBJ_ARRAY) {
            for (size_t i = 0; i < old_count; i++) {
                new_contents_array->array.items[1 + i] = contents->array.items[i];
            }
        } else if (contents && contents->type == TSPDF_OBJ_REF) {
            new_contents_array->array.items[1] = *contents;
        }
    } else {
        // Append-mode (overlay): wrap the ORIGINAL content in q..Q so the new
        // content starts from a pristine graphics state:
        // [q, ...old items, Q, new]
        const uint8_t q_bytes[] = "q\n";
        TspdfObj *q_stream = make_stream_obj(arena, q_bytes, 2);
        if (!q_stream) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }

        const uint8_t Q_bytes[] = "Q\n";
        TspdfObj *Q_stream = make_stream_obj(arena, Q_bytes, 2);
        if (!Q_stream) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }

        TspdfObj *new_stream = make_stream_obj(arena, final_content, final_len);
        if (!new_stream) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }

        uint32_t q_num = tspdf_register_new_obj(doc, q_stream);
        uint32_t Q_num = tspdf_register_new_obj(doc, Q_stream);
        uint32_t new_num = tspdf_register_new_obj(doc, new_stream);
        if (q_num == 0 || Q_num == 0 || new_num == 0) {
            tspdf_stream_destroy(stream);
            free(stream);
            return TSPDF_ERR_ALLOC;
        }

        TspdfObj *q_ref = make_ref_obj(arena, q_num);
        TspdfObj *Q_ref = make_ref_obj(arena, Q_num);
        TspdfObj *new_ref = make_ref_obj(arena, new_num);
        if (!q_ref || !Q_ref || !new_ref) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }

        if (contents && contents->type == TSPDF_OBJ_ARRAY) {
            // Already an array: [q_ref, ...old_items, Q_ref, new_ref]
            size_t old_count = contents->array.count;
            size_t total = old_count + 3; // q + old items + Q + new
            new_contents_array->array.items = tspdf_arena_alloc_zero(arena, total * sizeof(TspdfObj));
            if (!new_contents_array->array.items) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
            new_contents_array->array.count = total;

            new_contents_array->array.items[0] = *q_ref;
            for (size_t i = 0; i < old_count; i++) {
                new_contents_array->array.items[1 + i] = contents->array.items[i];
            }
            new_contents_array->array.items[1 + old_count] = *Q_ref;
            new_contents_array->array.items[2 + old_count] = *new_ref;
        } else if (contents && contents->type == TSPDF_OBJ_REF) {
            // Single ref: [q_ref, old_ref, Q_ref, new_ref]
            new_contents_array->array.items = tspdf_arena_alloc_zero(arena, 4 * sizeof(TspdfObj));
            if (!new_contents_array->array.items) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
            new_contents_array->array.count = 4;
            new_contents_array->array.items[0] = *q_ref;
            new_contents_array->array.items[1] = *contents;
            new_contents_array->array.items[2] = *Q_ref;
            new_contents_array->array.items[3] = *new_ref;
        } else {
            // No existing contents: just [new_ref]
            new_contents_array->array.items = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
            if (!new_contents_array->array.items) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
            new_contents_array->array.count = 1;
            new_contents_array->array.items[0] = *new_ref;
        }
    }

    // Replace /Contents in page dict
    bool replaced = false;
    for (size_t i = 0; i < page_dict->dict.count; i++) {
        if (strcmp(page_dict->dict.entries[i].key, "Contents") == 0) {
            page_dict->dict.entries[i].value = new_contents_array;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        // Add /Contents entry (rebuild entries array)
        size_t old_count = page_dict->dict.count;
        size_t new_count = old_count + 1;
        TspdfDictEntry *new_entries = tspdf_arena_alloc(arena, new_count * sizeof(TspdfDictEntry));
        if (!new_entries) { tspdf_stream_destroy(stream); free(stream); return TSPDF_ERR_ALLOC; }
        if (page_dict->dict.entries && old_count > 0) {
            memcpy(new_entries, page_dict->dict.entries, old_count * sizeof(TspdfDictEntry));
        }
        new_entries[old_count].key = tspdf_arena_alloc(arena, 9);
        memcpy(new_entries[old_count].key, "Contents", 9);
        new_entries[old_count].value = new_contents_array;
        page_dict->dict.entries = new_entries;
        page_dict->dict.count = new_count;
    }

    // 9. Bump PDF version if needed
    if (strcmp(doc->pdf_version, "1.4") < 0) {
        memcpy(doc->pdf_version, "1.4", 4);
    }

    // 10. Clean up the TspdfStream
    tspdf_stream_destroy(stream);
    free(stream);

    doc->modified = true;
    return TSPDF_OK;
}

TspdfError tspdf_page_end_content(TspdfReader *doc, size_t page_index,
                                  TspdfStream *stream, TspdfWriter *resource_owner) {
    return page_end_content_internal(doc, page_index, stream, resource_owner, false);
}

TspdfError tspdf_page_end_content_under(TspdfReader *doc, size_t page_index,
                                        TspdfStream *stream, TspdfWriter *resource_owner) {
    return page_end_content_internal(doc, page_index, stream, resource_owner, true);
}

void tspdf_page_abort_content(TspdfStream *stream) {
    if (stream) {
        tspdf_stream_destroy(stream);
        free(stream);
    }
}

// --- XObject resource registration (for imported form XObjects) ---

// Resolve an object that may be an indirect reference, looking in both the
// source xref and the new-object list.
static TspdfObj *resolve_maybe_ref(TspdfReader *doc, TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_REF) return obj;
    uint32_t num = obj->ref.num;
    if (num < doc->xref.count) {
        TspdfParser parser;
        tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
        return tspdf_xref_resolve(&doc->xref, &parser, num, doc->obj_cache, doc->crypt);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
}

// Set (replace or append) a dict entry, rebuilding the entries array in the
// arena when appending.
static bool dict_set(TspdfObj *dict, TspdfArena *arena, const char *key, TspdfObj *value) {
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            dict->dict.entries[i].value = value;
            return true;
        }
    }
    size_t old_count = dict->dict.count;
    TspdfDictEntry *entries = tspdf_arena_alloc(arena, (old_count + 1) * sizeof(TspdfDictEntry));
    if (!entries) return false;
    if (dict->dict.entries && old_count > 0) {
        memcpy(entries, dict->dict.entries, old_count * sizeof(TspdfDictEntry));
    }
    size_t klen = strlen(key);
    entries[old_count].key = tspdf_arena_alloc(arena, klen + 1);
    if (!entries[old_count].key) return false;
    memcpy(entries[old_count].key, key, klen + 1);
    entries[old_count].value = value;
    dict->dict.entries = entries;
    dict->dict.count = old_count + 1;
    return true;
}

// Get a sub-dict of `dict` under `key`, following an indirect reference;
// creates a direct empty dict when missing or unusable.
static TspdfObj *get_or_create_direct_subdict(TspdfReader *doc, TspdfObj *dict, const char *key) {
    TspdfArena *arena = &doc->arena;
    TspdfObj *sub = resolve_maybe_ref(doc, tspdf_dict_get(dict, key));
    if (sub && sub->type == TSPDF_OBJ_DICT) return sub;
    sub = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
    if (!sub) return NULL;
    sub->type = TSPDF_OBJ_DICT;
    if (!dict_set(dict, arena, key, sub)) return NULL;
    return sub;
}

const char *tspdf_page_add_xobject(TspdfReader *doc, size_t page_index, uint32_t xobj_num) {
    if (!doc || page_index >= doc->pages.count || xobj_num == 0) return NULL;
    if ((size_t)xobj_num >= doc->xref.count + doc->new_objs.count) return NULL;

    TspdfArena *arena = &doc->arena;
    TspdfObj *page_dict = doc->pages.pages[page_index].page_dict;
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) return NULL;

    // /Resources may be an indirect ref (possibly shared between pages);
    // resolve and mutate the resolved dict in place — we only append names.
    TspdfObj *resources = get_or_create_direct_subdict(doc, page_dict, "Resources");
    if (!resources) return NULL;

    TspdfObj *xobjects = get_or_create_direct_subdict(doc, resources, "XObject");
    if (!xobjects) return NULL;

    // Pick a fresh name.
    char buf[32];
    unsigned k = (unsigned)xobjects->dict.count + 1;
    for (;; k++) {
        snprintf(buf, sizeof(buf), "TsX%u", k);
        bool taken = false;
        for (size_t i = 0; i < xobjects->dict.count; i++) {
            if (strcmp(xobjects->dict.entries[i].key, buf) == 0) { taken = true; break; }
        }
        if (!taken) break;
    }

    TspdfObj *ref = make_ref_obj(arena, xobj_num);
    if (!ref) return NULL;
    if (!dict_set(xobjects, arena, buf, ref)) return NULL;

    // Return the arena-owned copy of the name (the dict entry key).
    for (size_t i = 0; i < xobjects->dict.count; i++) {
        if (strcmp(xobjects->dict.entries[i].key, buf) == 0) {
            doc->modified = true;
            return xobjects->dict.entries[i].key;
        }
    }
    return NULL;
}
