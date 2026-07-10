// Cross-document page import: wrap a source page as a /Form XObject in a
// destination document (the machinery behind `tspdf stamp`).
//
// Reuses the merge preparation path (tspdf_reader_make_streams_self_contained)
// to detach the source page's object graph from its backing buffer — which
// also decrypts encrypted stream data — then deep-copies the page's resources
// into the destination, importing every referenced object exactly once with a
// src→dst object-number map (this bounds hostile reference cycles).

#include "tspr_internal.h"
#include "tspr_doctree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// BBox coordinates are clamped to this magnitude: hostile MediaBoxes in the
// 1e300 range would otherwise flow into placement math downstream.
#define TSPDF_IMPORT_BBOX_LIMIT 1.0e7

// Cap on nodes visited while remapping refs in imported objects. Each source
// object is imported at most once (the map below), so in valid files the walk
// is bounded by the copied data anyway; this is a second fence.
#define TSPDF_IMPORT_NODE_BUDGET (4u * 1024u * 1024u)

// --- Small arena object constructors (mirrors tspr_content.c) ---

static TspdfObj *imp_obj(TspdfArena *a, TspdfObjType type) {
    TspdfObj *o = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = type;
    return o;
}

static TspdfObj *imp_name(TspdfArena *a, const char *str) {
    TspdfObj *o = imp_obj(a, TSPDF_OBJ_NAME);
    if (!o) return NULL;
    size_t len = strlen(str);
    o->string.data = tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    memcpy(o->string.data, str, len + 1);
    o->string.len = len;
    return o;
}

static TspdfObj *imp_int(TspdfArena *a, int64_t val) {
    TspdfObj *o = imp_obj(a, TSPDF_OBJ_INT);
    if (o) o->integer = val;
    return o;
}

static TspdfObj *imp_real(TspdfArena *a, double val) {
    TspdfObj *o = imp_obj(a, TSPDF_OBJ_REAL);
    if (o) o->real = val;
    return o;
}

static TspdfObj *imp_dict(TspdfArena *a, size_t capacity) {
    TspdfObj *o = imp_obj(a, TSPDF_OBJ_DICT);
    if (!o) return NULL;
    o->dict.entries = tspdf_arena_alloc_zero(a, capacity * sizeof(TspdfDictEntry));
    if (!o->dict.entries) return NULL;
    o->dict.count = 0;
    return o;
}

// Add to a dict created with enough capacity by imp_dict.
static bool imp_dict_add(TspdfObj *dict, TspdfArena *a, const char *key, TspdfObj *value) {
    if (!value) return false;
    size_t klen = strlen(key);
    char *k = tspdf_arena_alloc(a, klen + 1);
    if (!k) return false;
    memcpy(k, key, klen + 1);
    dict->dict.entries[dict->dict.count].key = k;
    dict->dict.entries[dict->dict.count].value = value;
    dict->dict.count++;
    return true;
}

// --- Worklist for the iterative ref remap (no recursion: hostile depth) ---

typedef struct {
    TspdfObj **items;
    size_t count;
    size_t capacity;
} ImpStack;

static bool imp_push(ImpStack *s, TspdfObj *obj) {
    if (!obj) return true;
    if (s->count >= s->capacity) {
        size_t cap = s->capacity == 0 ? 64 : s->capacity * 2;
        TspdfObj **arr = realloc(s->items, cap * sizeof(TspdfObj *));
        if (!arr) return false;
        s->items = arr;
        s->capacity = cap;
    }
    s->items[s->count++] = obj;
    return true;
}

// deep_copy mallocs stream bytes, but the reader only frees stream data
// reachable through obj_cache — new objects' buffers would leak. Move the
// bytes into the destination arena instead.
static bool imp_arena_stream_data(TspdfObj *stream_obj, TspdfArena *arena) {
    if (stream_obj->type != TSPDF_OBJ_STREAM) return true;
    if (!stream_obj->stream.data) {
        // Never materialized (e.g. resolve failure earlier): make it an empty
        // self-contained stream rather than leaving a dangling raw window.
        stream_obj->stream.len = 0;
        stream_obj->stream.raw_offset = 0;
        stream_obj->stream.raw_len = 0;
        stream_obj->stream.self_contained = true;
        stream_obj->stream.data = tspdf_arena_alloc(arena, 1);
        return stream_obj->stream.data != NULL;
    }
    size_t len = stream_obj->stream.len;
    uint8_t *moved = tspdf_arena_alloc(arena, len ? len : 1);
    if (!moved) return false;
    if (len > 0) memcpy(moved, stream_obj->stream.data, len);
    free(stream_obj->stream.data);
    stream_obj->stream.data = moved;
    stream_obj->stream.self_contained = true;
    stream_obj->stream.raw_offset = 0;
    stream_obj->stream.raw_len = len;
    return true;
}

// Walk `root` (already in dst's arena), importing every referenced source
// object into dst and rewriting the refs to the new numbers. map[] has one
// slot per source object number; 0 = not imported yet.
static TspdfError import_remap_refs(TspdfReader *dst, TspdfReader *src,
                                    TspdfParser *src_parser, TspdfObj *root,
                                    uint32_t *map) {
    ImpStack st = {0};
    TspdfError err = TSPDF_OK;
    if (!imp_push(&st, root)) return TSPDF_ERR_ALLOC;

    size_t budget = TSPDF_IMPORT_NODE_BUDGET;
    while (st.count > 0) {
        if (budget == 0) { err = TSPDF_ERR_PARSE; break; }
        budget--;

        TspdfObj *cur = st.items[--st.count];
        if (!cur) continue;

        switch (cur->type) {
            case TSPDF_OBJ_REF: {
                uint32_t n = cur->ref.num;
                if (n == 0 || n >= src->xref.count) {
                    cur->type = TSPDF_OBJ_NULL;
                    break;
                }
                if (map[n] != 0) {
                    cur->ref.num = map[n];
                    cur->ref.gen = 0;
                    break;
                }
                TspdfObj *resolved = tspdf_xref_resolve(&src->xref, src_parser, n,
                                                        src->obj_cache, src->crypt);
                if (!resolved) {
                    cur->type = TSPDF_OBJ_NULL;
                    break;
                }
                TspdfObj *copy = tspdf_obj_deep_copy(resolved, &dst->arena);
                if (!copy) { err = TSPDF_ERR_ALLOC; goto done; }
                if (!imp_arena_stream_data(copy, &dst->arena)) { err = TSPDF_ERR_ALLOC; goto done; }
                uint32_t num = tspdf_register_new_obj(dst, copy);
                if (num == 0) { err = TSPDF_ERR_ALLOC; goto done; }
                // Record the mapping BEFORE walking the copy: reference
                // cycles resolve against the map instead of recursing.
                map[n] = num;
                cur->ref.num = num;
                cur->ref.gen = 0;
                if (!imp_push(&st, copy)) { err = TSPDF_ERR_ALLOC; goto done; }
                break;
            }
            case TSPDF_OBJ_ARRAY:
                for (size_t i = 0; i < cur->array.count; i++) {
                    if (!imp_push(&st, &cur->array.items[i])) { err = TSPDF_ERR_ALLOC; goto done; }
                }
                break;
            case TSPDF_OBJ_DICT:
                for (size_t i = 0; i < cur->dict.count; i++) {
                    if (!imp_push(&st, cur->dict.entries[i].value)) { err = TSPDF_ERR_ALLOC; goto done; }
                }
                break;
            case TSPDF_OBJ_STREAM:
                if (!imp_push(&st, cur->stream.dict)) { err = TSPDF_ERR_ALLOC; goto done; }
                break;
            default:
                break;
        }
    }

done:
    free(st.items);
    return err;
}

// True when the object tree contains no indirect references (safe to copy
// verbatim across documents).
static bool obj_has_no_refs(TspdfObj *obj, int depth) {
    if (!obj || depth > 32) return obj == NULL;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            return false;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                if (!obj_has_no_refs(&obj->array.items[i], depth + 1)) return false;
            }
            return true;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                if (!obj_has_no_refs(obj->dict.entries[i].value, depth + 1)) return false;
            }
            return true;
        case TSPDF_OBJ_STREAM:
            return false;
        default:
            return true;
    }
}

// Collect the page's content streams (single stream or array of streams).
// Streams are already self-contained at this point. Returns the count, or
// SIZE_MAX on structural error. *out receives a malloc'd array.
static size_t collect_content_streams(TspdfReader *src, TspdfParser *parser,
                                      TspdfObj *page_dict, TspdfObj ***out) {
    *out = NULL;
    TspdfObj *contents = tspdf_dict_get(page_dict, "Contents");
    if (contents && contents->type == TSPDF_OBJ_REF && contents->ref.num < src->xref.count) {
        contents = tspdf_xref_resolve(&src->xref, parser, contents->ref.num,
                                      src->obj_cache, src->crypt);
    }
    if (!contents) return 0;

    if (contents->type == TSPDF_OBJ_STREAM) {
        TspdfObj **arr = malloc(sizeof(TspdfObj *));
        if (!arr) return SIZE_MAX;
        arr[0] = contents;
        *out = arr;
        return 1;
    }
    if (contents->type != TSPDF_OBJ_ARRAY) return 0;

    TspdfObj **arr = malloc((contents->array.count ? contents->array.count : 1) * sizeof(TspdfObj *));
    if (!arr) return SIZE_MAX;
    size_t n = 0;
    for (size_t i = 0; i < contents->array.count; i++) {
        TspdfObj *item = &contents->array.items[i];
        if (item->type == TSPDF_OBJ_REF && item->ref.num < src->xref.count) {
            item = tspdf_xref_resolve(&src->xref, parser, item->ref.num,
                                      src->obj_cache, src->crypt);
        }
        if (item && item->type == TSPDF_OBJ_STREAM) arr[n++] = item;
    }
    *out = arr;
    return n;
}

static double clamp_coord(double v) {
    if (v > TSPDF_IMPORT_BBOX_LIMIT) return TSPDF_IMPORT_BBOX_LIMIT;
    if (v < -TSPDF_IMPORT_BBOX_LIMIT) return -TSPDF_IMPORT_BBOX_LIMIT;
    if (v != v) return 0.0;  // NaN
    return v;
}

uint32_t tspdf_reader_import_page_xobject(TspdfReader *dst, TspdfReader *src,
                                          size_t src_page_index, double out_bbox[4],
                                          TspdfError *err) {
    if (err) *err = TSPDF_OK;
    if (!dst || !src) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return 0;
    }
    if (src_page_index >= src->pages.count) {
        if (err) *err = TSPDF_ERR_PAGE_RANGE;
        return 0;
    }

    TspdfReaderPage *page = &src->pages.pages[src_page_index];
    TspdfObj *page_dict = page->page_dict;
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) {
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return 0;
    }

    // BBox = source page MediaBox, clamped against hostile values.
    double bbox[4];
    for (int i = 0; i < 4; i++) bbox[i] = clamp_coord(page->media_box[i]);
    if (bbox[2] <= bbox[0] || bbox[3] <= bbox[1]) {
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return 0;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, src->data, src->data_len, &src->arena);

    // 1. Detach everything reachable from the page from the source buffer
    // (copies + decrypts stream bytes; same pass merge runs per page).
    if (src->xref.count > 0) {
        bool *visited = calloc(src->xref.count, sizeof(bool));
        if (!visited) {
            if (err) *err = TSPDF_ERR_ALLOC;
            return 0;
        }
        TspdfError prep = TSPDF_OK;
        if (page->obj_num > 0 && page->obj_num < src->xref.count) {
            prep = tspdf_reader_make_ref_self_contained(page->obj_num, src->data, src->data_len,
                                                        src->obj_cache, &src->xref, &parser,
                                                        src->crypt, visited, src->xref.count);
        }
        if (prep == TSPDF_OK) {
            prep = tspdf_reader_make_streams_self_contained(page_dict, src->data, src->data_len,
                                                            src->obj_cache, &src->xref, &parser,
                                                            src->crypt, visited, src->xref.count);
        }
        free(visited);
        if (prep != TSPDF_OK) {
            if (err) *err = prep;
            return 0;
        }
    }

    // 2. Gather the content stream(s).
    TspdfObj **streams = NULL;
    size_t stream_count = collect_content_streams(src, &parser, page_dict, &streams);
    if (stream_count == SIZE_MAX) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return 0;
    }

    // 3. Build the XObject stream data in dst's arena.
    //    Single stream with a ref-free filter chain: copy the encoded bytes
    //    verbatim and carry /Filter + /DecodeParms over (keeps compression).
    //    Otherwise: decode every stream and concatenate with newlines.
    uint8_t *data = NULL;
    size_t data_len = 0;
    TspdfObj *filter_copy = NULL;
    TspdfObj *parms_copy = NULL;

    if (stream_count == 1 && streams[0]->stream.data) {
        TspdfObj *filter = tspdf_dict_get(streams[0]->stream.dict, "Filter");
        TspdfObj *parms = tspdf_dict_get(streams[0]->stream.dict, "DecodeParms");
        if (obj_has_no_refs(filter, 0) && obj_has_no_refs(parms, 0)) {
            data_len = streams[0]->stream.len;
            data = tspdf_arena_alloc(&dst->arena, data_len ? data_len : 1);
            if (!data) { free(streams); if (err) *err = TSPDF_ERR_ALLOC; return 0; }
            if (data_len > 0) memcpy(data, streams[0]->stream.data, data_len);
            if (filter) filter_copy = tspdf_obj_deep_copy(filter, &dst->arena);
            if (parms) parms_copy = tspdf_obj_deep_copy(parms, &dst->arena);
            if ((filter && !filter_copy) || (parms && !parms_copy)) {
                free(streams);
                if (err) *err = TSPDF_ERR_ALLOC;
                return 0;
            }
        }
    }

    if (!data) {
        // Decode-and-concatenate path.
        size_t total = 0;
        uint8_t **decoded = NULL;
        size_t *decoded_len = NULL;
        if (stream_count > 0) {
            decoded = calloc(stream_count, sizeof(uint8_t *));
            decoded_len = calloc(stream_count, sizeof(size_t));
            if (!decoded || !decoded_len) {
                free(decoded); free(decoded_len); free(streams);
                if (err) *err = TSPDF_ERR_ALLOC;
                return 0;
            }
        }
        TspdfError derr = TSPDF_OK;
        for (size_t i = 0; i < stream_count; i++) {
            if (!streams[i]->stream.data) continue;
            derr = tspdf_stream_decode(streams[i]->stream.dict, streams[i]->stream.data,
                                       streams[i]->stream.len, &decoded[i], &decoded_len[i]);
            if (derr != TSPDF_OK) break;
            total += decoded_len[i] + 1;  // '\n' separator
        }
        if (derr != TSPDF_OK) {
            for (size_t i = 0; i < stream_count; i++) free(decoded ? decoded[i] : NULL);
            free(decoded); free(decoded_len); free(streams);
            if (err) *err = derr;
            return 0;
        }
        data = tspdf_arena_alloc(&dst->arena, total ? total : 1);
        if (!data) {
            for (size_t i = 0; i < stream_count; i++) free(decoded ? decoded[i] : NULL);
            free(decoded); free(decoded_len); free(streams);
            if (err) *err = TSPDF_ERR_ALLOC;
            return 0;
        }
        data_len = 0;
        for (size_t i = 0; i < stream_count; i++) {
            if (!decoded || !decoded[i]) continue;
            memcpy(data + data_len, decoded[i], decoded_len[i]);
            data_len += decoded_len[i];
            data[data_len++] = '\n';
            free(decoded[i]);
        }
        free(decoded);
        free(decoded_len);
    }
    free(streams);

    // 4. Deep-copy the page's resources into dst and import every object the
    // copy references. Resources may be inherited from the page tree.
    TspdfObj *res_src = tspdf_dict_get(page_dict, "Resources");
    if (res_src && res_src->type == TSPDF_OBJ_REF && res_src->ref.num < src->xref.count) {
        res_src = tspdf_xref_resolve(&src->xref, &parser, res_src->ref.num,
                                     src->obj_cache, src->crypt);
    }
    if (!res_src || res_src->type != TSPDF_OBJ_DICT) {
        // Walk up the page tree for inherited /Resources.
        TspdfObj *node = page_dict;
        res_src = NULL;
        size_t hops = src->xref.count + 32;
        while (node && node->type == TSPDF_OBJ_DICT && hops-- > 0) {
            TspdfObj *parent = tspdf_dict_get(node, "Parent");
            if (!parent || parent->type != TSPDF_OBJ_REF || parent->ref.num >= src->xref.count) break;
            node = tspdf_xref_resolve(&src->xref, &parser, parent->ref.num,
                                      src->obj_cache, src->crypt);
            if (!node || node->type != TSPDF_OBJ_DICT) break;
            TspdfObj *r = tspdf_dict_get(node, "Resources");
            if (r && r->type == TSPDF_OBJ_REF && r->ref.num < src->xref.count) {
                r = tspdf_xref_resolve(&src->xref, &parser, r->ref.num,
                                       src->obj_cache, src->crypt);
            }
            if (r && r->type == TSPDF_OBJ_DICT) { res_src = r; break; }
        }
        if (res_src) {
            // The inherited resources were not part of the page's
            // self-containment walk: detach their streams too.
            bool *visited = calloc(src->xref.count ? src->xref.count : 1, sizeof(bool));
            if (!visited) {
                if (err) *err = TSPDF_ERR_ALLOC;
                return 0;
            }
            TspdfError prep = tspdf_reader_make_streams_self_contained(res_src, src->data, src->data_len,
                                                                       src->obj_cache, &src->xref, &parser,
                                                                       src->crypt, visited, src->xref.count);
            free(visited);
            if (prep != TSPDF_OK) {
                if (err) *err = prep;
                return 0;
            }
        }
    }

    TspdfObj *res_copy;
    if (res_src && res_src->type == TSPDF_OBJ_DICT) {
        res_copy = tspdf_obj_deep_copy(res_src, &dst->arena);
    } else {
        res_copy = imp_dict(&dst->arena, 1);
    }
    if (!res_copy) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return 0;
    }

    if (src->xref.count > 0) {
        uint32_t *map = calloc(src->xref.count, sizeof(uint32_t));
        if (!map) {
            if (err) *err = TSPDF_ERR_ALLOC;
            return 0;
        }
        TspdfError rerr = import_remap_refs(dst, src, &parser, res_copy, map);
        free(map);
        if (rerr != TSPDF_OK) {
            if (err) *err = rerr;
            return 0;
        }
    }

    // 5. Assemble the form XObject.
    TspdfArena *arena = &dst->arena;
    TspdfObj *dict = imp_dict(arena, 8);
    if (!dict) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return 0;
    }

    TspdfObj *bbox_arr = imp_obj(arena, TSPDF_OBJ_ARRAY);
    if (!bbox_arr) { if (err) *err = TSPDF_ERR_ALLOC; return 0; }
    bbox_arr->array.items = tspdf_arena_alloc_zero(arena, 4 * sizeof(TspdfObj));
    if (!bbox_arr->array.items) { if (err) *err = TSPDF_ERR_ALLOC; return 0; }
    bbox_arr->array.count = 4;
    for (int i = 0; i < 4; i++) {
        TspdfObj *v = imp_real(arena, bbox[i]);
        if (!v) { if (err) *err = TSPDF_ERR_ALLOC; return 0; }
        bbox_arr->array.items[i] = *v;
    }

    bool ok = imp_dict_add(dict, arena, "Type", imp_name(arena, "XObject"))
           && imp_dict_add(dict, arena, "Subtype", imp_name(arena, "Form"))
           && imp_dict_add(dict, arena, "FormType", imp_int(arena, 1))
           && imp_dict_add(dict, arena, "BBox", bbox_arr)
           && imp_dict_add(dict, arena, "Resources", res_copy)
           && imp_dict_add(dict, arena, "Length", imp_int(arena, (int64_t)data_len));
    if (ok && filter_copy) ok = imp_dict_add(dict, arena, "Filter", filter_copy);
    if (ok && parms_copy) ok = imp_dict_add(dict, arena, "DecodeParms", parms_copy);
    if (!ok) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return 0;
    }

    TspdfObj *form = imp_obj(arena, TSPDF_OBJ_STREAM);
    if (!form) { if (err) *err = TSPDF_ERR_ALLOC; return 0; }
    form->stream.dict = dict;
    form->stream.data = data;
    form->stream.len = data_len;
    form->stream.raw_offset = 0;
    form->stream.raw_len = data_len;
    form->stream.self_contained = true;

    uint32_t num = tspdf_register_new_obj(dst, form);
    if (num == 0) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return 0;
    }

    if (strcmp(dst->pdf_version, "1.4") < 0) {
        memcpy(dst->pdf_version, "1.4", 4);
    }
    dst->modified = true;

    if (out_bbox) memcpy(out_bbox, bbox, sizeof(bbox));
    return num;
}
