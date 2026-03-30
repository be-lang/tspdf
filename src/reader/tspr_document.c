#include "tspr_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


TspdfObj *tspdf_dict_get(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return NULL;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0)
            return dict->dict.entries[i].value;
    }
    return NULL;
}

TspdfObj *tspdf_obj_deep_copy(TspdfObj *obj, TspdfArena *dst) {
    if (!obj) return NULL;

    TspdfObj *copy = (TspdfObj *)tspdf_arena_alloc(dst, sizeof(TspdfObj));
    if (!copy) return NULL;

    copy->type = obj->type;

    switch (obj->type) {
        case TSPDF_OBJ_NULL:
            break;
        case TSPDF_OBJ_BOOL:
            copy->boolean = obj->boolean;
            break;
        case TSPDF_OBJ_INT:
            copy->integer = obj->integer;
            break;
        case TSPDF_OBJ_REAL:
            copy->real = obj->real;
            break;
        case TSPDF_OBJ_REF:
            copy->ref.num = obj->ref.num;
            copy->ref.gen = obj->ref.gen;
            break;
        case TSPDF_OBJ_STRING:
        case TSPDF_OBJ_NAME: {
            copy->string.len = obj->string.len;
            copy->string.data = (uint8_t *)tspdf_arena_alloc(dst, obj->string.len + 1);
            if (!copy->string.data) return NULL;
            memcpy(copy->string.data, obj->string.data, obj->string.len);
            copy->string.data[obj->string.len] = '\0';
            break;
        }
        case TSPDF_OBJ_ARRAY: {
            copy->array.count = obj->array.count;
            copy->array.items = (TspdfObj *)tspdf_arena_alloc(dst, sizeof(TspdfObj) * obj->array.count);
            if (!copy->array.items && obj->array.count > 0) return NULL;
            for (size_t i = 0; i < obj->array.count; i++) {
                TspdfObj *item_copy = tspdf_obj_deep_copy(&obj->array.items[i], dst);
                if (!item_copy) return NULL;
                copy->array.items[i] = *item_copy;
            }
            break;
        }
        case TSPDF_OBJ_DICT: {
            copy->dict.count = obj->dict.count;
            copy->dict.entries = (TspdfDictEntry *)tspdf_arena_alloc(dst, sizeof(TspdfDictEntry) * obj->dict.count);
            if (!copy->dict.entries && obj->dict.count > 0) return NULL;
            for (size_t i = 0; i < obj->dict.count; i++) {
                size_t klen = strlen(obj->dict.entries[i].key);
                copy->dict.entries[i].key = (char *)tspdf_arena_alloc(dst, klen + 1);
                if (!copy->dict.entries[i].key) return NULL;
                memcpy(copy->dict.entries[i].key, obj->dict.entries[i].key, klen + 1);
                copy->dict.entries[i].value = tspdf_obj_deep_copy(obj->dict.entries[i].value, dst);
                if (!copy->dict.entries[i].value) return NULL;
            }
            break;
        }
        case TSPDF_OBJ_STREAM: {
            copy->stream.dict = tspdf_obj_deep_copy(obj->stream.dict, dst);
            if (!copy->stream.dict) return NULL;
            copy->stream.raw_offset = obj->stream.raw_offset;
            copy->stream.raw_len = obj->stream.raw_len;
            if (obj->stream.data != NULL) {
                copy->stream.data = (uint8_t *)malloc(obj->stream.raw_len);
                if (!copy->stream.data) return NULL;
                memcpy(copy->stream.data, obj->stream.data, obj->stream.raw_len);
            } else {
                copy->stream.data = NULL;
            }
            copy->stream.len = obj->stream.len;
            copy->stream.self_contained = obj->stream.self_contained;
            break;
        }
    }

    return copy;
}

static TspdfReader *open_internal(const uint8_t *data, size_t len,
                                    const char *password, TspdfError *err) {
    // 1. Validate args
    if (!data || len == 0) {
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return NULL;
    }

    // 2. Verify %PDF- header
    if (len < 5 || memcmp(data, "%PDF-", 5) != 0) {
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return NULL;
    }

    // 3. Extract version string
    char pdf_version[8] = {0};
    size_t vi = 0;
    for (size_t i = 5; i < len && vi < sizeof(pdf_version) - 1; i++) {
        if (data[i] == '\r' || data[i] == '\n') break;
        pdf_version[vi++] = (char)data[i];
    }

    // 4. Allocate TspdfReader
    TspdfReader *doc = (TspdfReader *)calloc(1, sizeof(TspdfReader));
    if (!doc) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // 5. Create arena
    size_t arena_cap = len * 2;
    if (arena_cap < 262144) arena_cap = 262144;  // 256KB minimum
    doc->arena = tspdf_arena_create(arena_cap);
    if (!doc->arena.first) {
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    doc->data = data;
    doc->data_len = len;
    doc->owns_data = false;
    memcpy(doc->pdf_version, pdf_version, sizeof(pdf_version));

    // 6. Create parser, parse xref
    TspdfParser parser;
    tspdf_parser_init(&parser, data, len, &doc->arena);

    TspdfError xref_err = tspdf_xref_parse(&parser, &doc->xref);
    if (xref_err != TSPDF_OK) {
        tspdf_arena_destroy(&doc->arena);
        free(doc);
        if (err) *err = xref_err;
        return NULL;
    }

    // Allocate obj_cache (needed by resolve)
    doc->obj_cache = (TspdfObj **)tspdf_arena_alloc_zero(&doc->arena,
                        sizeof(TspdfObj *) * doc->xref.count);
    if (!doc->obj_cache) {
        tspdf_arena_destroy(&doc->arena);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // 7. Check for /Encrypt in trailer and initialize decryption
    TspdfObj *encrypt = tspdf_dict_get(doc->xref.trailer, "Encrypt");
    if (encrypt) {
        // Resolve the encrypt dict if it's a reference
        TspdfObj *encrypt_dict = encrypt;
        if (encrypt->type == TSPDF_OBJ_REF) {
            encrypt_dict = tspdf_xref_resolve(&doc->xref, &parser, encrypt->ref.num,
                                              doc->obj_cache, NULL);
        }
        if (!encrypt_dict || (encrypt_dict->type != TSPDF_OBJ_DICT)) {
            tspdf_reader_destroy(doc);
            if (err) *err = TSPDF_ERR_PARSE;
            return NULL;
        }

        doc->crypt = (TspdfCrypt *)malloc(sizeof(TspdfCrypt));
        if (!doc->crypt) {
            tspdf_reader_destroy(doc);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        memset(doc->crypt, 0, sizeof(TspdfCrypt));

        const char *try_password = password ? password : "";
        TspdfError crypt_err = tspdf_crypt_init(doc->crypt, encrypt_dict,
                                               doc->xref.trailer, try_password);
        if (crypt_err != TSPDF_OK) {
            free(doc->crypt);
            doc->crypt = NULL;
            tspdf_reader_destroy(doc);
            if (err) *err = password ? TSPDF_ERR_BAD_PASSWORD : TSPDF_ERR_ENCRYPTED;
            return NULL;
        }
    }

    // 8. Resolve /Root catalog from trailer
    TspdfObj *root_ref = tspdf_dict_get(doc->xref.trailer, "Root");
    if (!root_ref || root_ref->type != TSPDF_OBJ_REF) {
        tspdf_reader_destroy(doc);
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    doc->catalog = tspdf_xref_resolve(&doc->xref, &parser, root_ref->ref.num,
                                      doc->obj_cache, doc->crypt);
    if (!doc->catalog || doc->catalog->type != TSPDF_OBJ_DICT) {
        tspdf_reader_destroy(doc);
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Load page tree
    TspdfError pages_err = tspdf_pages_load(&parser, &doc->xref, doc->obj_cache,
                                           doc->catalog, &doc->pages, doc->crypt);
    if (pages_err != TSPDF_OK) {
        tspdf_reader_destroy(doc);
        if (err) *err = pages_err;
        return NULL;
    }

    if (err) *err = TSPDF_OK;
    return doc;
}

TspdfReader *tspdf_reader_open(const uint8_t *data, size_t len, TspdfError *err) {
    return open_internal(data, len, NULL, err);
}

static TspdfReader *open_file_internal(const char *path, const char *password, TspdfError *err) {
    if (!path) {
        if (err) *err = TSPDF_ERR_IO;
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) *err = TSPDF_ERR_IO;
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (read_bytes != (size_t)size) {
        free(buf);
        if (err) *err = TSPDF_ERR_IO;
        return NULL;
    }

    TspdfReader *doc = open_internal(buf, (size_t)size, password, err);
    if (!doc) {
        free(buf);
        return NULL;
    }

    doc->owns_data = true;
    return doc;
}

TspdfReader *tspdf_reader_open_file(const char *path, TspdfError *err) {
    return open_file_internal(path, NULL, err);
}

TspdfReader *tspdf_reader_open_with_password(const uint8_t *data, size_t len,
                                                const char *password, TspdfError *err) {
    return open_internal(data, len, password, err);
}

TspdfReader *tspdf_reader_open_file_with_password(const char *path,
                                                     const char *password, TspdfError *err) {
    return open_file_internal(path, password, err);
}

// Free malloc'd stream data from obj_cache entries
static void free_stream_data(TspdfReader *doc) {
    if (!doc->obj_cache) return;
    for (size_t i = 0; i < doc->xref.count; i++) {
        TspdfObj *obj = doc->obj_cache[i];
        if (obj && obj->type == TSPDF_OBJ_STREAM && obj->stream.data) {
            free(obj->stream.data);
            obj->stream.data = NULL;
        }
    }
}

void tspdf_reader_destroy(TspdfReader *doc) {
    if (!doc) return;
    free_stream_data(doc);
    if (doc->crypt) {
        free(doc->crypt->file_id);
        free(doc->crypt);
    }
    if (doc->metadata) {
        free(doc->metadata->title);
        free(doc->metadata->author);
        free(doc->metadata->subject);
        free(doc->metadata->keywords);
        free(doc->metadata->creator);
        free(doc->metadata->producer);
        free(doc->metadata);
    }
    free(doc->new_objs.objs);
    tspdf_arena_destroy(&doc->arena);
    if (doc->owns_data) {
        free((void *)doc->data);
    }
    free(doc);
}

uint32_t tspdf_register_new_obj(TspdfReader *doc, TspdfObj *obj) {
    if (doc->new_objs.count >= doc->new_objs.capacity) {
        size_t new_cap = doc->new_objs.capacity == 0 ? 16 : doc->new_objs.capacity * 2;
        TspdfObj **new_arr = realloc(doc->new_objs.objs, new_cap * sizeof(TspdfObj *));
        if (!new_arr) return 0;
        doc->new_objs.objs = new_arr;
        doc->new_objs.capacity = new_cap;
    }
    uint32_t obj_num = (uint32_t)(doc->xref.count + doc->new_objs.count);
    doc->new_objs.objs[doc->new_objs.count] = obj;
    doc->new_objs.count++;
    return obj_num;
}

size_t tspdf_reader_page_count(const TspdfReader *doc) {
    return doc ? doc->pages.count : 0;
}

TspdfReaderPage *tspdf_reader_get_page(const TspdfReader *doc, size_t index) {
    if (!doc || index >= doc->pages.count) return NULL;
    return &doc->pages.pages[index];
}

// Helper: create a new document from selected pages of a source document.
// The new document shares the source's data buffer (source must outlive result).
static TspdfReader *create_doc_from_pages(TspdfReader *src, const size_t *page_indices,
                                            size_t count, TspdfError *err) {
    // Validate indices
    for (size_t i = 0; i < count; i++) {
        if (page_indices[i] >= src->pages.count) {
            if (err) *err = TSPDF_ERR_PARSE;
            return NULL;
        }
    }

    TspdfReader *doc = (TspdfReader *)calloc(1, sizeof(TspdfReader));
    if (!doc) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Create arena proportional to source
    size_t arena_cap = src->arena.total_allocated;
    if (arena_cap < 65536) arena_cap = 65536;
    doc->arena = tspdf_arena_create(arena_cap);
    if (!doc->arena.first) {
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Share source buffer
    doc->data = src->data;
    doc->data_len = src->data_len;
    doc->owns_data = false;
    memcpy(doc->pdf_version, src->pdf_version, sizeof(doc->pdf_version));

    // Copy xref from source (needed by serializer for object resolution)
    doc->xref.count = src->xref.count;
    doc->xref.entries = src->xref.entries;
    doc->xref.trailer = src->xref.trailer;

    // Copy obj_cache from source (so we can update page entries)
    doc->obj_cache = (TspdfObj **)tspdf_arena_alloc_zero(&doc->arena,
                        sizeof(TspdfObj *) * src->xref.count);
    if (!doc->obj_cache) {
        tspdf_arena_destroy(&doc->arena);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    memcpy(doc->obj_cache, src->obj_cache, sizeof(TspdfObj *) * src->xref.count);

    // Build page list with deep-copied page dicts
    doc->pages.count = count;
    doc->pages.pages = (TspdfReaderPage *)tspdf_arena_alloc_zero(&doc->arena, sizeof(TspdfReaderPage) * count);
    if (!doc->pages.pages) {
        tspdf_arena_destroy(&doc->arena);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        TspdfReaderPage *src_page = &src->pages.pages[page_indices[i]];
        doc->pages.pages[i].obj_num = src_page->obj_num;
        doc->pages.pages[i].page_dict = tspdf_obj_deep_copy(src_page->page_dict, &doc->arena);
        if (!doc->pages.pages[i].page_dict) {
            tspdf_arena_destroy(&doc->arena);
            free(doc);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        memcpy(doc->pages.pages[i].media_box, src_page->media_box, sizeof(double) * 4);
        doc->pages.pages[i].rotate = src_page->rotate;

        // Update obj_cache to point to our deep-copied page dict
        if (src_page->obj_num < src->xref.count) {
            doc->obj_cache[src_page->obj_num] = doc->pages.pages[i].page_dict;
        }
    }

    doc->modified = true;
    if (err) *err = TSPDF_OK;
    return doc;
}

TspdfReader *tspdf_reader_extract(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err) {
    if (!doc || !pages || count == 0) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }
    return create_doc_from_pages(doc, pages, count, err);
}

TspdfReader *tspdf_reader_delete(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err) {
    if (!doc || !pages) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Build list of pages to keep (all except the ones in `pages`)
    size_t total = doc->pages.count;
    bool *exclude = (bool *)calloc(total, sizeof(bool));
    if (!exclude) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (pages[i] < total) exclude[pages[i]] = true;
    }

    size_t keep_count = 0;
    for (size_t i = 0; i < total; i++) {
        if (!exclude[i]) keep_count++;
    }

    size_t *keep = (size_t *)malloc(sizeof(size_t) * (keep_count > 0 ? keep_count : 1));
    if (!keep) {
        free(exclude);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    size_t ki = 0;
    for (size_t i = 0; i < total; i++) {
        if (!exclude[i]) keep[ki++] = i;
    }
    free(exclude);

    TspdfReader *result = create_doc_from_pages(doc, keep, keep_count, err);
    free(keep);
    return result;
}

// Helper: set or update /Rotate in a page dict
static void set_rotate_in_dict(TspdfObj *page_dict, int angle, TspdfArena *arena) {
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) return;

    // Look for existing /Rotate entry
    for (size_t i = 0; i < page_dict->dict.count; i++) {
        if (strcmp(page_dict->dict.entries[i].key, "Rotate") == 0) {
            page_dict->dict.entries[i].value->type = TSPDF_OBJ_INT;
            page_dict->dict.entries[i].value->integer = angle;
            return;
        }
    }

    // No existing /Rotate — add a new entry by creating a new entries array
    size_t old_count = page_dict->dict.count;
    size_t new_count = old_count + 1;
    TspdfDictEntry *new_entries = (TspdfDictEntry *)tspdf_arena_alloc(arena, sizeof(TspdfDictEntry) * new_count);
    if (!new_entries) return;

    memcpy(new_entries, page_dict->dict.entries, sizeof(TspdfDictEntry) * old_count);

    // Add /Rotate entry
    char *key = (char *)tspdf_arena_alloc(arena, 7);
    if (!key) return;
    memcpy(key, "Rotate", 7);
    TspdfObj *val = (TspdfObj *)tspdf_arena_alloc(arena, sizeof(TspdfObj));
    if (!val) return;
    val->type = TSPDF_OBJ_INT;
    val->integer = angle;
    new_entries[old_count].key = key;
    new_entries[old_count].value = val;

    page_dict->dict.entries = new_entries;
    page_dict->dict.count = new_count;
}

TspdfReader *tspdf_reader_rotate(TspdfReader *doc, const size_t *pages, size_t count, int angle, TspdfError *err) {
    if (!doc || !pages) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Validate angle: must be a multiple of 90
    if (angle % 90 != 0) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Extract all pages
    size_t total = doc->pages.count;
    size_t *all = (size_t *)malloc(sizeof(size_t) * total);
    if (!all) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    for (size_t i = 0; i < total; i++) all[i] = i;

    TspdfReader *result = create_doc_from_pages(doc, all, total, err);
    free(all);
    if (!result) return NULL;

    // Now modify /Rotate in the specified pages
    for (size_t i = 0; i < count; i++) {
        if (pages[i] >= result->pages.count) continue;
        TspdfReaderPage *page = &result->pages.pages[pages[i]];
        set_rotate_in_dict(page->page_dict, angle, &result->arena);
        page->rotate = angle;
    }

    if (err) *err = TSPDF_OK;
    return result;
}

TspdfReader *tspdf_reader_reorder(TspdfReader *doc, const size_t *order, size_t count, TspdfError *err) {
    if (!doc || !order || count == 0) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }
    return create_doc_from_pages(doc, order, count, err);
}

// Recursively walk an object tree and make all streams self-contained
static void make_streams_self_contained(TspdfObj *obj, TspdfArena *dst, const uint8_t *src_data,
                                         TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                         bool *visited, size_t xref_count);

static void make_ref_self_contained(uint32_t obj_num, TspdfArena *dst, const uint8_t *src_data,
                                     TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                     bool *visited, size_t xref_count) {
    if (obj_num >= xref_count) return;
    if (visited[obj_num]) return;
    visited[obj_num] = true;

    TspdfObj *resolved = tspdf_xref_resolve(xref, parser, obj_num, cache, NULL);
    if (!resolved) return;

    if (resolved->type == TSPDF_OBJ_STREAM) {
        // Make this stream self-contained
        if (resolved->stream.data == NULL && src_data != NULL) {
            uint8_t *copy = (uint8_t *)malloc(resolved->stream.raw_len);
            if (copy) {
                memcpy(copy, src_data + resolved->stream.raw_offset, resolved->stream.raw_len);
                resolved->stream.data = copy;
                resolved->stream.len = resolved->stream.raw_len;
                resolved->stream.self_contained = true;
                resolved->stream.raw_offset = 0;
            }
        }
        // Also recurse into stream dict
        make_streams_self_contained(resolved->stream.dict, dst, src_data, cache, xref, parser, visited, xref_count);
    } else {
        make_streams_self_contained(resolved, dst, src_data, cache, xref, parser, visited, xref_count);
    }
}

static void make_streams_self_contained(TspdfObj *obj, TspdfArena *dst, const uint8_t *src_data,
                                         TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                         bool *visited, size_t xref_count) {
    if (!obj) return;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            make_ref_self_contained(obj->ref.num, dst, src_data, cache, xref, parser, visited, xref_count);
            break;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++)
                make_streams_self_contained(&obj->array.items[i], dst, src_data, cache, xref, parser, visited, xref_count);
            break;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++)
                make_streams_self_contained(obj->dict.entries[i].value, dst, src_data, cache, xref, parser, visited, xref_count);
            break;
        case TSPDF_OBJ_STREAM:
            make_streams_self_contained(obj->stream.dict, dst, src_data, cache, xref, parser, visited, xref_count);
            break;
        default:
            break;
    }
}

TspdfReader *tspdf_reader_merge(TspdfReader **docs, size_t count, TspdfError *err) {
    if (!docs || count == 0) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Count total pages
    size_t total_pages = 0;
    for (size_t d = 0; d < count; d++) {
        if (!docs[d]) {
            if (err) *err = TSPDF_ERR_PARSE;
            return NULL;
        }
        total_pages += docs[d]->pages.count;
    }

    // Calculate total xref count needed: sum of all source xrefs
    // We remap object numbers so they don't collide: doc[i] objects get offset
    size_t total_xref = 1; // obj 0 (free)
    size_t *xref_offsets = (size_t *)calloc(count, sizeof(size_t));
    if (!xref_offsets) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    for (size_t d = 0; d < count; d++) {
        xref_offsets[d] = total_xref;
        total_xref += docs[d]->xref.count;
    }

    TspdfReader *doc = (TspdfReader *)calloc(1, sizeof(TspdfReader));
    if (!doc) {
        free(xref_offsets);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // TspdfArena: sum of all source arenas
    size_t arena_cap = 0;
    for (size_t d = 0; d < count; d++) {
        arena_cap += docs[d]->arena.total_allocated;
    }
    if (arena_cap < 131072) arena_cap = 131072;
    doc->arena = tspdf_arena_create(arena_cap);
    if (!doc->arena.first) {
        free(xref_offsets);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Merged doc has no single source buffer
    doc->data = NULL;
    doc->data_len = 0;
    doc->owns_data = false;
    memcpy(doc->pdf_version, docs[0]->pdf_version, sizeof(doc->pdf_version));

    // Build combined xref
    doc->xref.count = total_xref;
    doc->xref.entries = (TspdfReaderXrefEntry *)tspdf_arena_alloc_zero(&doc->arena,
                            sizeof(TspdfReaderXrefEntry) * total_xref);
    if (!doc->xref.entries) {
        tspdf_arena_destroy(&doc->arena);
        free(xref_offsets);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Build combined obj_cache
    doc->obj_cache = (TspdfObj **)tspdf_arena_alloc_zero(&doc->arena,
                        sizeof(TspdfObj *) * total_xref);
    if (!doc->obj_cache) {
        tspdf_arena_destroy(&doc->arena);
        free(xref_offsets);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // For each source doc, copy xref entries and remap obj_cache
    for (size_t d = 0; d < count; d++) {
        TspdfReader *src = docs[d];
        size_t offset = xref_offsets[d];

        // First make all streams self-contained in the source doc
        TspdfParser parser;
        tspdf_parser_init(&parser, src->data, src->data_len, &src->arena);

        bool *visited = (bool *)calloc(src->xref.count, sizeof(bool));
        if (!visited) {
            tspdf_arena_destroy(&doc->arena);
            free(xref_offsets);
            free(doc);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }

        // Walk all pages to make their streams self-contained
        for (size_t p = 0; p < src->pages.count; p++) {
            TspdfReaderPage *page = &src->pages.pages[p];
            if (page->obj_num < src->xref.count) {
                make_ref_self_contained(page->obj_num, &src->arena, src->data,
                                        src->obj_cache, &src->xref, &parser,
                                        visited, src->xref.count);
            }
            make_streams_self_contained(page->page_dict, &src->arena, src->data,
                                        src->obj_cache, &src->xref, &parser,
                                        visited, src->xref.count);
        }
        free(visited);

        // Copy xref entries with offset
        for (size_t i = 1; i < src->xref.count; i++) {
            doc->xref.entries[offset + i] = src->xref.entries[i];
        }

        // Deep copy cached objects with remapped refs
        for (size_t i = 1; i < src->xref.count; i++) {
            if (src->obj_cache[i]) {
                TspdfObj *copy = tspdf_obj_deep_copy(src->obj_cache[i], &doc->arena);
                if (copy) {
                    doc->obj_cache[offset + i] = copy;
                }
            }
        }
    }

    // Remap all refs in the merged obj_cache
    for (size_t d = 0; d < count; d++) {
        TspdfReader *src = docs[d];
        size_t offset = xref_offsets[d];
        for (size_t i = 1; i < src->xref.count; i++) {
            if (doc->obj_cache[offset + i]) {
                // Remap refs in this object
                // We need a recursive remap function
                // Simple approach: walk and remap
                // (implemented inline below via stack-based approach)
            }
        }
    }

    // Recursive ref remapping helper - we need to remap all refs in each doc's objects
    // For objects from doc d, ref num N becomes N + xref_offsets[d]
    // We do this per-source-doc
    for (size_t d = 0; d < count; d++) {
        size_t offset = xref_offsets[d];
        size_t src_xref_count = docs[d]->xref.count;

        for (size_t i = 1; i < src_xref_count; i++) {
            TspdfObj *obj = doc->obj_cache[offset + i];
            if (!obj) continue;

            // Use a simple iterative stack to remap refs
            // Stack of TspdfObj* to process
            size_t stack_cap = 256;
            size_t stack_len = 0;
            TspdfObj **stack = (TspdfObj **)malloc(sizeof(TspdfObj *) * stack_cap);
            if (!stack) continue;
            stack[stack_len++] = obj;

            while (stack_len > 0) {
                TspdfObj *cur = stack[--stack_len];
                if (!cur) continue;

                switch (cur->type) {
                    case TSPDF_OBJ_REF:
                        if (cur->ref.num < src_xref_count) {
                            cur->ref.num += (uint32_t)offset;
                        }
                        break;
                    case TSPDF_OBJ_ARRAY:
                        for (size_t j = 0; j < cur->array.count; j++) {
                            if (stack_len >= stack_cap) {
                                stack_cap *= 2;
                                TspdfObj **new_stack = (TspdfObj **)realloc(stack, sizeof(TspdfObj *) * stack_cap);
                                if (!new_stack) goto done_stack;
                                stack = new_stack;
                            }
                            stack[stack_len++] = &cur->array.items[j];
                        }
                        break;
                    case TSPDF_OBJ_DICT:
                        for (size_t j = 0; j < cur->dict.count; j++) {
                            if (stack_len >= stack_cap) {
                                stack_cap *= 2;
                                TspdfObj **new_stack = (TspdfObj **)realloc(stack, sizeof(TspdfObj *) * stack_cap);
                                if (!new_stack) goto done_stack;
                                stack = new_stack;
                            }
                            stack[stack_len++] = cur->dict.entries[j].value;
                        }
                        break;
                    case TSPDF_OBJ_STREAM:
                        if (stack_len >= stack_cap) {
                            stack_cap *= 2;
                            TspdfObj **new_stack = (TspdfObj **)realloc(stack, sizeof(TspdfObj *) * stack_cap);
                            if (!new_stack) goto done_stack;
                            stack = new_stack;
                        }
                        stack[stack_len++] = cur->stream.dict;
                        break;
                    default:
                        break;
                }
            }
            done_stack:
            free(stack);
        }
    }

    // Build page list
    doc->pages.count = total_pages;
    doc->pages.pages = (TspdfReaderPage *)tspdf_arena_alloc_zero(&doc->arena, sizeof(TspdfReaderPage) * total_pages);
    if (!doc->pages.pages) {
        tspdf_arena_destroy(&doc->arena);
        free(xref_offsets);
        free(doc);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    size_t pi = 0;
    for (size_t d = 0; d < count; d++) {
        TspdfReader *src = docs[d];
        size_t offset = xref_offsets[d];
        for (size_t p = 0; p < src->pages.count; p++) {
            TspdfReaderPage *src_page = &src->pages.pages[p];
            doc->pages.pages[pi].obj_num = (uint32_t)(src_page->obj_num + offset);
            doc->pages.pages[pi].page_dict = doc->obj_cache[src_page->obj_num + offset];
            if (!doc->pages.pages[pi].page_dict) {
                // Deep copy the page dict if not already in cache
                doc->pages.pages[pi].page_dict = tspdf_obj_deep_copy(src_page->page_dict, &doc->arena);
            }
            memcpy(doc->pages.pages[pi].media_box, src_page->media_box, sizeof(double) * 4);
            doc->pages.pages[pi].rotate = src_page->rotate;
            pi++;
        }
    }

    // Build a minimal trailer
    doc->xref.trailer = (TspdfObj *)tspdf_arena_alloc_zero(&doc->arena, sizeof(TspdfObj));
    if (doc->xref.trailer) {
        doc->xref.trailer->type = TSPDF_OBJ_DICT;
        doc->xref.trailer->dict.count = 0;
        doc->xref.trailer->dict.entries = NULL;
    }

    free(xref_offsets);
    doc->modified = true;
    if (err) *err = TSPDF_OK;
    return doc;
}

TspdfError tspdf_reader_save(TspdfReader *doc, const char *path) {
    if (!doc || !path) return TSPDF_ERR_IO;

    uint8_t *buf = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory(doc, &buf, &len);
    if (err != TSPDF_OK) return err;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return TSPDF_ERR_IO;
    }

    size_t written = fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);

    if (written != len) return TSPDF_ERR_IO;
    return TSPDF_OK;
}

TspdfSaveOptions tspdf_save_options_default(void) {
    TspdfSaveOptions opts = {0};
    opts.update_producer = true;
    return opts;
}

TspdfError tspdf_reader_save_to_memory(TspdfReader *doc, uint8_t **out, size_t *out_len) {
    if (!doc || !out || !out_len) return TSPDF_ERR_PARSE;
    TspdfSaveOptions opts = tspdf_save_options_default();
    return tspdf_serialize_with_options(doc, out, out_len, &opts);
}

TspdfError tspdf_reader_save_to_memory_with_options(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                     const TspdfSaveOptions *opts) {
    if (!doc || !out || !out_len || !opts) return TSPDF_ERR_PARSE;
    return tspdf_serialize_with_options(doc, out, out_len, opts);
}

TspdfError tspdf_reader_save_with_options(TspdfReader *doc, const char *path,
                                          const TspdfSaveOptions *opts) {
    if (!doc || !path || !opts) return TSPDF_ERR_IO;
    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, opts);
    if (err != TSPDF_OK) return err;
    FILE *f = fopen(path, "wb");
    if (!f) { free(out); return TSPDF_ERR_IO; }
    fwrite(out, 1, out_len, f);
    fclose(f);
    free(out);
    return TSPDF_OK;
}

TspdfError tspdf_reader_save_to_memory_encrypted(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                  const char *user_pass, const char *owner_pass,
                                                  uint32_t permissions, int key_bits) {
    if (!doc || !out || !out_len) return TSPDF_ERR_PARSE;
    TspdfCrypt crypt = {0};
    TspdfError err = tspdf_crypt_encrypt_init(&crypt, user_pass, owner_pass, permissions, key_bits);
    if (err != TSPDF_OK) return err;
    err = tspdf_serialize_encrypted(doc, &crypt, out, out_len);
    free(crypt.file_id);
    return err;
}

TspdfError tspdf_reader_save_encrypted(TspdfReader *doc, const char *path,
                                        const char *user_pass, const char *owner_pass,
                                        uint32_t permissions, int key_bits) {
    if (!doc || !path) return TSPDF_ERR_IO;
    uint8_t *buf = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory_encrypted(doc, &buf, &len,
                                                            user_pass, owner_pass, permissions, key_bits);
    if (err != TSPDF_OK) return err;
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return TSPDF_ERR_IO; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return TSPDF_OK;
}
