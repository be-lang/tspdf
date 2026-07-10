#include "tspr_internal.h"
#include "tspr_doctree.h"
#include "tspr_attach.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TSPDF_HEADER_SCAN_LIMIT 1024

static bool find_pdf_header(const uint8_t *data, size_t len, size_t *out_offset) {
    if (!data || !out_offset || len < 5) {
        return false;
    }

    // Fast path: ISO 32000 lets readers require the header in the first 1024
    // bytes, which is where conforming files put it.
    size_t limit = len < TSPDF_HEADER_SCAN_LIMIT ? len : TSPDF_HEADER_SCAN_LIMIT;
    for (size_t i = 0; i + 5 <= limit; i++) {
        if (memcmp(data + i, "%PDF-", 5) == 0) {
            *out_offset = i;
            return true;
        }
    }

    // Slow path: some real-world files carry a larger transfer prefix (mail
    // wrappers, HTTP dumps, etc.). Scan the rest of the buffer before failing.
    for (size_t i = limit >= 5 ? limit - 4 : 0; i + 5 <= len; i++) {
        if (memcmp(data + i, "%PDF-", 5) == 0) {
            *out_offset = i;
            return true;
        }
    }

    return false;
}

static void free_reader_crypt(TspdfCrypt *crypt) {
    if (!crypt) return;
    free(crypt->file_id);
    // Wipe key material (file key + cached expanded schedules) before the
    // struct returns to the heap.
    memset(crypt, 0, sizeof(*crypt));
    free(crypt);
}

// Clone a reader's crypt for a derived document (extract, nup, ...) so its
// save preserves the source encryption. The clone owns its file_id copy;
// src_encrypt_dict still points into the SOURCE document's arena, which the
// derived-document lifetime rules already require to outlive the clone
// (source must outlive the derived document until it is saved).
TspdfCrypt *tspdf_crypt_clone(const TspdfCrypt *src) {
    if (!src) return NULL;

    TspdfCrypt *copy = (TspdfCrypt *)malloc(sizeof(TspdfCrypt));
    if (!copy) return NULL;
    *copy = *src;
    copy->file_id = NULL;

    if (src->file_id && src->file_id_len > 0) {
        copy->file_id = (uint8_t *)malloc(src->file_id_len);
        if (!copy->file_id) {
            free(copy);
            return NULL;
        }
        memcpy(copy->file_id, src->file_id, src->file_id_len);
    }

    return copy;
}

TspdfObj *tspdf_dict_get(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return NULL;
    for (size_t i = dict->dict.count; i > 0; i--) {
        size_t idx = i - 1;
        if (strcmp(dict->dict.entries[idx].key, key) == 0)
            return dict->dict.entries[idx].value;
    }
    return NULL;
}

// True when the trailer's /Root resolves to a /Type /Catalog dict through the
// current xref. Used to decide whether a parsed xref is usable or whether the
// reader must fall back to an object-scan rebuild. Uses a throwaway cache so it
// does not pollute the document's own object cache.
static bool trailer_root_resolves_to_catalog(TspdfReaderXref *xref,
                                              TspdfParser *parser,
                                              TspdfObj **scratch_cache) {
    if (!xref->trailer || xref->trailer->type != TSPDF_OBJ_DICT) {
        return false;
    }
    TspdfObj *root = tspdf_dict_get(xref->trailer, "Root");
    if (!root) {
        return false;
    }

    TspdfObj *catalog = NULL;
    if (root->type == TSPDF_OBJ_DICT) {
        catalog = root;
    } else if (root->type == TSPDF_OBJ_REF) {
        catalog = tspdf_xref_resolve(xref, parser, root->ref.num, scratch_cache, NULL);
    } else {
        return false;
    }

    if (!catalog || catalog->type != TSPDF_OBJ_DICT) {
        return false;
    }
    TspdfObj *type = tspdf_dict_get(catalog, "Type");
    return type && type->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)type->string.data, "Catalog") == 0;
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
                copy->stream.data = (uint8_t *)malloc(obj->stream.len ? obj->stream.len : 1);
                if (!copy->stream.data) return NULL;
                if (obj->stream.len > 0) {
                    memcpy(copy->stream.data, obj->stream.data, obj->stream.len);
                }
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

    // 2. Verify %PDF- header. ISO 32000 allows readers to find the header
    // within the first 1024 bytes; some real-world PDFs have transfer prefixes.
    size_t header_offset = 0;
    if (!find_pdf_header(data, len, &header_offset)) {
        if (err) *err = TSPDF_ERR_INVALID_PDF;
        return NULL;
    }

    // 3. Extract version string
    char pdf_version[8] = {0};
    size_t vi = 0;
    for (size_t i = header_offset + 5; i < len && vi < sizeof(pdf_version) - 1; i++) {
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

    // 6b. When the "%PDF-" header sits behind a transfer prefix, a classic xref
    // parses cleanly but every row offset is shifted by the prefix length, so
    // no object resolves. In that specific case, rebuild the xref by scanning
    // the buffer for objects (which uses absolute offsets). This is gated on a
    // non-zero header offset so that a normal file with a deliberately corrupt
    // xref (wrong object number/generation) is still rejected rather than
    // silently repaired. Encrypted files are exempt — their catalog cannot be
    // resolved until the crypt layer is initialized below.
    if (header_offset > 0 &&
        !tspdf_dict_get(doc->xref.trailer, "Encrypt") &&
        !trailer_root_resolves_to_catalog(&doc->xref, &parser, doc->obj_cache)) {
        TspdfReaderXref rebuilt = {0};
        TspdfParser scan_parser;
        tspdf_parser_init(&scan_parser, data, len, &doc->arena);
        if (tspdf_xref_reconstruct_by_scan(&scan_parser, &rebuilt) == TSPDF_OK) {
            doc->xref = rebuilt;
            doc->obj_cache = (TspdfObj **)tspdf_arena_alloc_zero(&doc->arena,
                                sizeof(TspdfObj *) * doc->xref.count);
            if (!doc->obj_cache) {
                tspdf_arena_destroy(&doc->arena);
                free(doc);
                if (err) *err = TSPDF_ERR_ALLOC;
                return NULL;
            }
        }
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
            // tspdf_reader_destroy frees doc->crypt (including file_id)
            tspdf_reader_destroy(doc);
            if (err) *err = password ? TSPDF_ERR_BAD_PASSWORD : TSPDF_ERR_ENCRYPTED;
            return NULL;
        }

        // Keep the (still raw — resolved before crypt init) source /Encrypt
        // dict so saves can preserve the document's encryption verbatim.
        doc->crypt->src_encrypt_dict = encrypt_dict;
        doc->crypt->src_encrypt_num =
            encrypt->type == TSPDF_OBJ_REF ? encrypt->ref.num : 0;
    }

    // 8. Resolve /Root catalog from trailer
    TspdfObj *root = tspdf_dict_get(doc->xref.trailer, "Root");
    if (!root) {
        tspdf_reader_destroy(doc);
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    if (root->type == TSPDF_OBJ_REF) {
        doc->catalog = tspdf_xref_resolve(&doc->xref, &parser, root->ref.num,
                                          doc->obj_cache, doc->crypt);
    } else if (root->type == TSPDF_OBJ_DICT) {
        doc->catalog = root;
    } else {
        tspdf_reader_destroy(doc);
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

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
    free_reader_crypt(doc->crypt);
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

const char *tspdf_reader_pdf_version(const TspdfReader *doc) {
    if (!doc) return "";
    // The catalog /Version name overrides the header (PDF 32000-1 §7.7.2).
    // Parsed names are NUL-terminated in the arena, so it can be returned
    // directly.
    if (doc->catalog) {
        TspdfObj *v = tspdf_dict_get(doc->catalog, "Version");
        // The spec wants a direct name here, but resolve an indirect
        // reference like every other catalog lookup does instead of silently
        // ignoring it. Resolution populates the shared object cache, so the
        // const on doc is logical, not physical.
        if (v && v->type == TSPDF_OBJ_REF && v->ref.num < doc->xref.count) {
            TspdfReader *mdoc = (TspdfReader *)doc;
            TspdfParser parser;
            tspdf_parser_init(&parser, mdoc->data, mdoc->data_len, &mdoc->arena);
            v = tspdf_xref_resolve(&mdoc->xref, &parser, v->ref.num,
                                   mdoc->obj_cache, mdoc->crypt);
        }
        if (v && v->type == TSPDF_OBJ_NAME && v->string.data && v->string.len > 0)
            return (const char *)v->string.data;
    }
    return doc->pdf_version;
}

bool tspdf_reader_encryption_info(const TspdfReader *doc, int *revision,
                                  const char **algorithm) {
    if (!doc || !doc->crypt) return false;
    const TspdfCrypt *c = doc->crypt;
    if (revision) *revision = c->revision;
    if (algorithm) {
        if (c->use_aes)
            *algorithm = (c->key_len == 32) ? "AES-256" : "AES-128";
        else
            *algorithm = (c->key_len == 5) ? "RC4-40"
                       : (c->key_len == 16) ? "RC4-128" : "RC4";
    }
    return true;
}

bool tspdf_reader_encryption_permissions(const TspdfReader *doc,
                                         uint32_t *permissions) {
    if (!doc || !doc->crypt) return false;
    if (permissions) *permissions = doc->crypt->permissions;
    return true;
}

static bool catalog_has(const TspdfReader *doc, const char *key) {
    if (!doc || !doc->catalog) return false;
    TspdfObj *v = tspdf_dict_get(doc->catalog, key);
    return v && v->type != TSPDF_OBJ_NULL;
}

bool tspdf_reader_has_outlines(const TspdfReader *doc) {
    return catalog_has(doc, "Outlines");
}

bool tspdf_reader_has_acroform(const TspdfReader *doc) {
    return catalog_has(doc, "AcroForm");
}

static bool dict_has_key(TspdfObj *dict, const char *key) {
    return tspdf_dict_get(dict, key) != NULL;
}

static char *arena_strdup(TspdfArena *arena, const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)tspdf_arena_alloc(arena, len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static TspdfError dict_add_deep_copy(TspdfObj *dict, const char *key,
                                     TspdfObj *value, TspdfArena *arena) {
    if (!dict || dict->type != TSPDF_OBJ_DICT || !key || !value) {
        return TSPDF_OK;
    }
    if (dict_has_key(dict, key)) {
        return TSPDF_OK;
    }

    size_t old_count = dict->dict.count;
    if (old_count == SIZE_MAX) {
        return TSPDF_ERR_ALLOC;
    }

    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(arena,
        sizeof(TspdfDictEntry) * (old_count + 1));
    if (!entries) return TSPDF_ERR_ALLOC;
    if (old_count > 0) {
        memcpy(entries, dict->dict.entries, sizeof(TspdfDictEntry) * old_count);
    }

    char *key_copy = arena_strdup(arena, key);
    if (!key_copy) return TSPDF_ERR_ALLOC;

    TspdfObj *value_copy = tspdf_obj_deep_copy(value, arena);
    if (!value_copy) return TSPDF_ERR_ALLOC;

    entries[old_count].key = key_copy;
    entries[old_count].value = value_copy;
    dict->dict.entries = entries;
    dict->dict.count = old_count + 1;
    return TSPDF_OK;
}

static TspdfObj *resolve_doc_obj(TspdfReader *doc, TspdfParser *parser, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }
    if (obj->ref.num >= doc->xref.count) {
        return NULL;
    }
    return tspdf_xref_resolve(&doc->xref, parser, obj->ref.num, doc->obj_cache, doc->crypt);
}

static TspdfObj *find_inherited_page_attr(TspdfReader *doc, TspdfParser *parser,
                                          TspdfObj *page_dict, const char *key) {
    if (!doc || !page_dict || page_dict->type != TSPDF_OBJ_DICT) {
        return NULL;
    }

    size_t max_depth = doc->xref.count > 0 ? doc->xref.count + 32 : 1024;
    if (max_depth < 1024) max_depth = 1024;
    TspdfObj *cur = page_dict;

    for (size_t depth = 0; cur && cur->type == TSPDF_OBJ_DICT && depth < max_depth; depth++) {
        TspdfObj *value = tspdf_dict_get(cur, key);
        if (value) {
            return value;
        }

        TspdfObj *parent = tspdf_dict_get(cur, "Parent");
        if (!parent) {
            return NULL;
        }
        cur = resolve_doc_obj(doc, parser, parent);
    }

    return NULL;
}

static TspdfError materialize_inherited_page_attr(TspdfReader *src, TspdfParser *parser,
                                                  TspdfObj *src_page_dict,
                                                  TspdfObj *target_page_dict,
                                                  TspdfArena *arena,
                                                  const char *key) {
    if (!target_page_dict || target_page_dict->type != TSPDF_OBJ_DICT ||
        dict_has_key(target_page_dict, key)) {
        return TSPDF_OK;
    }

    TspdfObj *value = find_inherited_page_attr(src, parser, src_page_dict, key);
    if (!value) {
        return TSPDF_OK;
    }

    return dict_add_deep_copy(target_page_dict, key, value, arena);
}

static TspdfError materialize_inherited_page_attrs(TspdfReader *src, TspdfParser *parser,
                                                   TspdfObj *src_page_dict,
                                                   TspdfReaderPage *target_page,
                                                   TspdfArena *arena) {
    TspdfError err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                                     target_page->page_dict, arena, "Resources");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "MediaBox");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "CropBox");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "BleedBox");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "TrimBox");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "ArtBox");
    if (err != TSPDF_OK) return err;
    err = materialize_inherited_page_attr(src, parser, src_page_dict,
                                          target_page->page_dict, arena, "Rotate");
    if (err != TSPDF_OK) return err;
    return materialize_inherited_page_attr(src, parser, src_page_dict,
                                           target_page->page_dict, arena, "UserUnit");
}

// Helper: create a new document from selected pages of a source document.
// The new document shares the source's data buffer (source must outlive result).
static TspdfReader *create_doc_from_pages(TspdfReader *src, const size_t *page_indices,
                                            size_t count, TspdfError *err) {
    // Validate indices
    for (size_t i = 0; i < count; i++) {
        if (page_indices[i] >= src->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
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

    TspdfParser parser;
    tspdf_parser_init(&parser, src->data, src->data_len, &src->arena);

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
        if (src_page->has_crop_box) {
            memcpy(doc->pages.pages[i].crop_box, src_page->crop_box, sizeof(double) * 4);
            doc->pages.pages[i].has_crop_box = true;
        }
        doc->pages.pages[i].user_unit = src_page->user_unit > 0.0 ? src_page->user_unit : 1.0;
        doc->pages.pages[i].rotate = src_page->rotate;

        TspdfError inherit_err = materialize_inherited_page_attrs(src, &parser,
                                                                  src_page->page_dict,
                                                                  &doc->pages.pages[i],
                                                                  &doc->arena);
        if (inherit_err != TSPDF_OK) {
            tspdf_arena_destroy(&doc->arena);
            free(doc);
            if (err) *err = inherit_err;
            return NULL;
        }

        // Update obj_cache to point to our deep-copied page dict
        if (src_page->obj_num < src->xref.count) {
            doc->obj_cache[src_page->obj_num] = doc->pages.pages[i].page_dict;
        }
    }

    if (src->crypt) {
        doc->crypt = tspdf_crypt_clone(src->crypt);
        if (!doc->crypt) {
            tspdf_arena_destroy(&doc->arena);
            free(doc);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
    }

    doc->modified = true;
    if (err) *err = TSPDF_OK;
    return doc;
}

// Catalog entries dropped when extracting pages into a new document. These
// document-level trees (structure tree, outlines, destination/name trees,
// page labels, form fields) reference the source's full page set and would
// keep the entire object graph reachable in the extracted file. Outlines,
// AcroForm, and PageLabels are re-attached afterwards, remapped to the kept
// pages, by tspdf_doctree_extract_attach; the rest stays dropped.
static bool extract_drops_catalog_key(const char *key) {
    static const char *const dropped[] = {
        "StructTreeRoot", "Outlines", "Dests", "Names", "PageLabels",
        "AcroForm", NULL
    };
    for (size_t i = 0; dropped[i] != NULL; i++) {
        if (strcmp(key, dropped[i]) == 0) return true;
    }
    return false;
}

// Give an extracted document its own catalog: a copy of the source catalog
// without the document-level trees above. Without this the serializer falls
// back to the shared source catalog and everything those trees reference is
// collected into the output.
static TspdfError set_extract_catalog(TspdfReader *src, TspdfReader *dst) {
    TspdfObj *src_catalog = src->catalog;
    if (!src_catalog && src->xref.trailer) {
        TspdfObj *root = tspdf_dict_get(src->xref.trailer, "Root");
        if (root && root->type == TSPDF_OBJ_DICT) {
            src_catalog = root;
        } else if (root && root->type == TSPDF_OBJ_REF &&
                   root->ref.num < src->xref.count) {
            TspdfParser parser;
            tspdf_parser_init(&parser, src->data, src->data_len, &src->arena);
            src_catalog = tspdf_xref_resolve(&src->xref, &parser, root->ref.num,
                                             src->obj_cache, src->crypt);
        }
    }
    if (!src_catalog || src_catalog->type != TSPDF_OBJ_DICT) {
        return TSPDF_OK;
    }

    TspdfObj *copy = (TspdfObj *)tspdf_arena_alloc_zero(&dst->arena, sizeof(TspdfObj));
    if (!copy) return TSPDF_ERR_ALLOC;
    copy->type = TSPDF_OBJ_DICT;
    copy->dict.count = 0;
    copy->dict.entries = NULL;

    size_t kept = 0;
    for (size_t i = 0; i < src_catalog->dict.count; i++) {
        if (!extract_drops_catalog_key(src_catalog->dict.entries[i].key)) kept++;
    }
    if (kept > 0) {
        copy->dict.entries = (TspdfDictEntry *)tspdf_arena_alloc(&dst->arena,
            sizeof(TspdfDictEntry) * kept);
        if (!copy->dict.entries) return TSPDF_ERR_ALLOC;
        size_t ci = 0;
        for (size_t i = 0; i < src_catalog->dict.count; i++) {
            const char *key = src_catalog->dict.entries[i].key;
            if (extract_drops_catalog_key(key)) continue;
            char *key_copy = arena_strdup(&dst->arena, key);
            if (!key_copy) return TSPDF_ERR_ALLOC;
            TspdfObj *value_copy = tspdf_obj_deep_copy(src_catalog->dict.entries[i].value,
                                                       &dst->arena);
            if (!value_copy) return TSPDF_ERR_ALLOC;
            copy->dict.entries[ci].key = key_copy;
            copy->dict.entries[ci].value = value_copy;
            ci++;
        }
        copy->dict.count = ci;
    }

    dst->catalog = copy;
    return TSPDF_OK;
}

TspdfReader *tspdf_reader_extract(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err) {
    if (!doc || !pages || count == 0) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    TspdfReader *result = create_doc_from_pages(doc, pages, count, err);
    if (!result) return NULL;

    TspdfError cat_err = set_extract_catalog(doc, result);
    if (cat_err != TSPDF_OK) {
        tspdf_reader_destroy(result);
        if (err) *err = cat_err;
        return NULL;
    }

    // Re-attach the outline subtrees and form fields that belong to the
    // kept pages (set_extract_catalog dropped the document-level trees).
    TspdfError trees_err = tspdf_doctree_extract_attach(doc, result);
    if (trees_err != TSPDF_OK) {
        tspdf_reader_destroy(result);
        if (err) *err = trees_err;
        return NULL;
    }

    // Attachments are document-level, not page-level: every one survives.
    TspdfError attach_err = tspdf_attach_extract_attach(doc, result);
    if (attach_err != TSPDF_OK) {
        tspdf_reader_destroy(result);
        if (err) *err = attach_err;
        return NULL;
    }
    return result;
}

TspdfReader *tspdf_reader_delete(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err) {
    if (!doc || !pages) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Reject out-of-range page indices up front (a delete that silently
    // ignores them would hide caller bugs and mismatch extract/reorder).
    for (size_t i = 0; i < count; i++) {
        if (pages[i] >= doc->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
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
static int normalize_rotation(int angle) {
    int normalized = angle % 360;
    if (normalized < 0) {
        normalized += 360;
    }
    return normalized;
}

static void set_rotate_in_dict(TspdfObj *page_dict, int angle, TspdfArena *arena) {
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) return;

    angle = normalize_rotation(angle);

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

    // Reject out-of-range page indices before doing any work
    for (size_t i = 0; i < count; i++) {
        if (pages[i] >= doc->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
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
        page->rotate = normalize_rotation(angle);
    }

    if (err) *err = TSPDF_OK;
    return result;
}

// Build a 4-element numeric array object [a b c d] in `arena`.
static TspdfObj *make_box_array(TspdfArena *arena, const double v[4]) {
    TspdfObj *arr = (TspdfObj *)tspdf_arena_alloc(arena, sizeof(TspdfObj));
    if (!arr) return NULL;
    arr->type = TSPDF_OBJ_ARRAY;
    arr->array.count = 4;
    arr->array.items = (TspdfObj *)tspdf_arena_alloc(arena, sizeof(TspdfObj) * 4);
    if (!arr->array.items) return NULL;
    for (int i = 0; i < 4; i++) {
        arr->array.items[i].type = TSPDF_OBJ_REAL;
        arr->array.items[i].real = v[i];
    }
    return arr;
}

// Set (replace or append) a box entry `key` = [x0 y0 x1 y1] in a page dict.
static bool set_box_in_dict(TspdfObj *page_dict, TspdfArena *arena,
                            const char *key, const double v[4]) {
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *arr = make_box_array(arena, v);
    if (!arr) return false;

    for (size_t i = 0; i < page_dict->dict.count; i++) {
        if (strcmp(page_dict->dict.entries[i].key, key) == 0) {
            page_dict->dict.entries[i].value = arr;
            return true;
        }
    }
    size_t old_count = page_dict->dict.count;
    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(arena,
                                sizeof(TspdfDictEntry) * (old_count + 1));
    if (!entries) return false;
    if (page_dict->dict.entries && old_count > 0) {
        memcpy(entries, page_dict->dict.entries, sizeof(TspdfDictEntry) * old_count);
    }
    size_t klen = strlen(key);
    char *k = (char *)tspdf_arena_alloc(arena, klen + 1);
    if (!k) return false;
    memcpy(k, key, klen + 1);
    entries[old_count].key = k;
    entries[old_count].value = arr;
    page_dict->dict.entries = entries;
    page_dict->dict.count = old_count + 1;
    return true;
}

// Core: each pages[i] gets boxes[i*4 .. i*4+3] as its CropBox (absolute, in
// default user space), intersected with the page MediaBox. When `boxes` is
// NULL, `single` is used for every listed page instead.
static TspdfReader *set_cropbox_impl(TspdfReader *doc, const size_t *pages,
                                     size_t count, const double *boxes,
                                     const double single[4], TspdfError *err) {
    if (!doc || !pages || count == 0) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const double *b = boxes ? &boxes[i * 4] : single;
        if (b[2] <= b[0] || b[3] <= b[1]) {
            if (err) *err = TSPDF_ERR_INVALID_ARG;
            return NULL;
        }
        if (pages[i] >= doc->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
    }

    size_t total = doc->pages.count;
    size_t *all = (size_t *)malloc(sizeof(size_t) * total);
    if (!all) { if (err) *err = TSPDF_ERR_ALLOC; return NULL; }
    for (size_t i = 0; i < total; i++) all[i] = i;
    TspdfReader *result = create_doc_from_pages(doc, all, total, err);
    free(all);
    if (!result) return NULL;

    for (size_t i = 0; i < count; i++) {
        const double *b = boxes ? &boxes[i * 4] : single;
        TspdfReaderPage *page = &result->pages.pages[pages[i]];
        const double *mb = page->media_box;
        // Intersect the requested box with the page MediaBox.
        double cb[4];
        cb[0] = b[0] < mb[0] ? mb[0] : b[0];
        cb[1] = b[1] < mb[1] ? mb[1] : b[1];
        cb[2] = b[2] > mb[2] ? mb[2] : b[2];
        cb[3] = b[3] > mb[3] ? mb[3] : b[3];
        if (cb[2] <= cb[0] || cb[3] <= cb[1]) {
            // The clamped box collapsed (requested box lies outside the media).
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_INVALID_ARG;
            return NULL;
        }
        if (!set_box_in_dict(page->page_dict, &result->arena, "CropBox", cb)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        memcpy(page->crop_box, cb, sizeof(cb));
        page->has_crop_box = true;
    }
    if (err) *err = TSPDF_OK;
    return result;
}

TspdfReader *tspdf_reader_set_cropbox(TspdfReader *doc, const size_t *pages,
                                      size_t count, const double box[4],
                                      TspdfError *err) {
    if (!box) { if (err) *err = TSPDF_ERR_INVALID_ARG; return NULL; }
    return set_cropbox_impl(doc, pages, count, NULL, box, err);
}

TspdfReader *tspdf_reader_set_cropboxes(TspdfReader *doc, const size_t *pages,
                                        size_t count, const double *boxes,
                                        TspdfError *err) {
    if (!boxes) { if (err) *err = TSPDF_ERR_INVALID_ARG; return NULL; }
    return set_cropbox_impl(doc, pages, count, boxes, NULL, err);
}

TspdfReader *tspdf_reader_scale(TspdfReader *doc, const size_t *pages,
                                size_t count, double sx, double sy,
                                TspdfError *err) {
    if (!doc || !pages || count == 0) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    if (!(sx > 0.0) || !(sy > 0.0)) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (pages[i] >= doc->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
    }

    size_t total = doc->pages.count;
    size_t *all = (size_t *)malloc(sizeof(size_t) * total);
    if (!all) { if (err) *err = TSPDF_ERR_ALLOC; return NULL; }
    for (size_t i = 0; i < total; i++) all[i] = i;
    TspdfReader *result = create_doc_from_pages(doc, all, total, err);
    free(all);
    if (!result) return NULL;

    for (size_t i = 0; i < count; i++) {
        size_t pi = pages[i];
        TspdfReaderPage *page = &result->pages.pages[pi];

        // Wrap the existing content in "q sx 0 0 sy 0 0 cm ... Q" so it scales
        // about the default-user-space origin, matching the scaled boxes.
        char prefix[64];
        int plen = snprintf(prefix, sizeof(prefix), "q %.6g 0 0 %.6g 0 0 cm\n", sx, sy);
        if (plen < 0 || plen >= (int)sizeof(prefix)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_INVALID_ARG;
            return NULL;
        }
        const char *suffix = "\nQ\n";
        TspdfError werr = tspdf_page_wrap_content(result, pi,
                                                  (const uint8_t *)prefix, (size_t)plen,
                                                  (const uint8_t *)suffix, strlen(suffix));
        if (werr != TSPDF_OK) {
            tspdf_reader_destroy(result);
            if (err) *err = werr;
            return NULL;
        }

        // Scale MediaBox and (if present) CropBox by the same factors.
        double mb[4];
        memcpy(mb, page->media_box, sizeof(mb));
        double smb[4] = { mb[0] * sx, mb[1] * sy, mb[2] * sx, mb[3] * sy };
        if (!set_box_in_dict(page->page_dict, &result->arena, "MediaBox", smb)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        memcpy(page->media_box, smb, sizeof(smb));

        TspdfObj *cbobj = tspdf_dict_get(page->page_dict, "CropBox");
        if (cbobj && cbobj->type == TSPDF_OBJ_ARRAY && cbobj->array.count >= 4) {
            double cb[4];
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                TspdfObj *it = &cbobj->array.items[k];
                if (it->type == TSPDF_OBJ_INT) cb[k] = (double)it->integer;
                else if (it->type == TSPDF_OBJ_REAL) cb[k] = it->real;
                else { ok = false; break; }
            }
            if (ok) {
                double scb[4] = { cb[0] * sx, cb[1] * sy, cb[2] * sx, cb[3] * sy };
                if (set_box_in_dict(page->page_dict, &result->arena, "CropBox", scb)) {
                    memcpy(page->crop_box, scb, sizeof(scb));
                    page->has_crop_box = true;
                }
            }
        }
    }
    if (err) *err = TSPDF_OK;
    return result;
}

// Remove a dict entry by key (shifts the remaining entries down).
static void remove_from_dict(TspdfObj *dict, const char *key) {
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

TspdfReader *tspdf_reader_resize_to(TspdfReader *doc, const size_t *pages,
                                    size_t count, double target_w, double target_h,
                                    TspdfError *err) {
    if (!doc || !pages || count == 0) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    if (!(target_w > 0.0) || !(target_h > 0.0)) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (pages[i] >= doc->pages.count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
    }

    size_t total = doc->pages.count;
    size_t *all = (size_t *)malloc(sizeof(size_t) * total);
    if (!all) { if (err) *err = TSPDF_ERR_ALLOC; return NULL; }
    for (size_t i = 0; i < total; i++) all[i] = i;
    TspdfReader *result = create_doc_from_pages(doc, all, total, err);
    free(all);
    if (!result) return NULL;

    for (size_t i = 0; i < count; i++) {
        size_t pi = pages[i];
        TspdfReaderPage *page = &result->pages.pages[pi];

        double pw = page->media_box[2] - page->media_box[0];
        double ph = page->media_box[3] - page->media_box[1];
        if (!(pw > 0.0) || !(ph > 0.0)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_INVALID_ARG;
            return NULL;
        }

        // Orientation rule: the page as VIEWED (after /Rotate, which is kept)
        // must be the target size oriented to match the source's VIEWED
        // orientation — a landscape-viewed page comes out target-landscape, a
        // portrait-viewed page target-portrait, so nothing is flipped and the
        // aspect-fit below never clips. Since /Rotate applies the same axis
        // swap to input and output, "viewed orientation preserved" collapses
        // to "stored orientation preserved": the stored target box (fit_w x
        // fit_h) is the requested size with width/height swapped, if needed,
        // to match the stored source box's orientation.
        double fit_w = target_w, fit_h = target_h;
        if ((pw > ph) != (fit_w > fit_h)) {
            double tmp = fit_w; fit_w = fit_h; fit_h = tmp;
        }

        double s = fit_w / pw;
        double s2 = fit_h / ph;
        if (s2 < s) s = s2;

        // Translate so the scaled content (measured from the MediaBox origin)
        // is centered within the new [0 0 fit_w fit_h] media, then
        // undo the MediaBox origin so content maps from (0,0).
        double content_w = pw * s;
        double content_h = ph * s;
        double tx = (fit_w - content_w) / 2.0 - page->media_box[0] * s;
        double ty = (fit_h - content_h) / 2.0 - page->media_box[1] * s;

        char prefix[96];
        int plen = snprintf(prefix, sizeof(prefix),
                            "q %.6g 0 0 %.6g %.6g %.6g cm\n", s, s, tx, ty);
        if (plen < 0 || plen >= (int)sizeof(prefix)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_INVALID_ARG;
            return NULL;
        }
        const char *suffix = "\nQ\n";
        TspdfError werr = tspdf_page_wrap_content(result, pi,
                                                  (const uint8_t *)prefix, (size_t)plen,
                                                  (const uint8_t *)suffix, strlen(suffix));
        if (werr != TSPDF_OK) {
            tspdf_reader_destroy(result);
            if (err) *err = werr;
            return NULL;
        }

        // fit_w/fit_h are the stored (unrotated) extents of the target box,
        // which is also the space the content-wrap above centered in.
        double mb[4] = { 0.0, 0.0, fit_w, fit_h };
        if (!set_box_in_dict(page->page_dict, &result->arena, "MediaBox", mb)) {
            tspdf_reader_destroy(result);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        memcpy(page->media_box, mb, sizeof(mb));
        // The old CropBox no longer maps to the resized media.
        remove_from_dict(page->page_dict, "CropBox");
        page->has_crop_box = false;
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

// Recursively walk an object tree and make source-backed streams self-contained
// before copying objects into a merged document with no single backing buffer.
// Non-static (declared in tspr_doctree.h): the doctree merge preparation
// walks outline/AcroForm graphs with the same machinery.

static bool stream_dict_type_is(TspdfObj *obj, const char *type_name) {
    if (!obj || obj->type != TSPDF_OBJ_STREAM || !type_name) {
        return false;
    }
    TspdfObj *type_val = tspdf_dict_get(obj->stream.dict, "Type");
    return type_val && type_val->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)type_val->string.data, type_name) == 0;
}

static TspdfError copy_stream_source(TspdfObj *obj, const uint8_t *src_data, size_t src_len,
                                     TspdfCrypt *crypt, uint32_t obj_num, uint16_t gen) {
    if (!obj || obj->type != TSPDF_OBJ_STREAM) return TSPDF_OK;
    if (obj->stream.self_contained && obj->stream.data != NULL) return TSPDF_OK;

    if (!src_data || obj->stream.raw_offset > src_len ||
        obj->stream.raw_len > src_len - obj->stream.raw_offset) {
        return TSPDF_ERR_PARSE;
    }

    const uint8_t *raw = src_data + obj->stream.raw_offset;
    size_t raw_len = obj->stream.raw_len;
    uint8_t *copy = NULL;
    size_t copy_len = raw_len;

    if (crypt && obj_num > 0 && !stream_dict_type_is(obj, "XRef") && raw_len > 0) {
        copy = tspdf_crypt_decrypt_stream(crypt, obj_num, gen, raw, raw_len, &copy_len);
        if (!copy) {
            return TSPDF_ERR_PARSE;
        }
    } else {
        size_t alloc_len = raw_len ? raw_len : 1;
        copy = (uint8_t *)malloc(alloc_len);
        if (!copy) return TSPDF_ERR_ALLOC;
        if (raw_len > 0) {
            memcpy(copy, raw, raw_len);
        }
    }

    obj->stream.data = copy;
    obj->stream.len = copy_len;
    obj->stream.raw_len = copy_len;
    obj->stream.self_contained = true;
    obj->stream.raw_offset = 0;
    return TSPDF_OK;
}

TspdfError tspdf_reader_make_ref_self_contained(uint32_t obj_num, const uint8_t *src_data, size_t src_len,
                                                TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                                TspdfCrypt *crypt, bool *visited, size_t xref_count) {
    if (obj_num >= xref_count) return TSPDF_OK;
    if (visited[obj_num]) return TSPDF_OK;
    visited[obj_num] = true;

    TspdfObj *resolved = tspdf_xref_resolve(xref, parser, obj_num, cache, crypt);
    if (!resolved) return TSPDF_OK;

    if (resolved->type == TSPDF_OBJ_STREAM) {
        uint16_t gen = obj_num < xref->count ? xref->entries[obj_num].gen : 0;
        TspdfError err = copy_stream_source(resolved, src_data, src_len, crypt, obj_num, gen);
        if (err != TSPDF_OK) return err;
        return tspdf_reader_make_streams_self_contained(resolved->stream.dict, src_data, src_len,
                                                        cache, xref, parser, crypt, visited, xref_count);
    }

    return tspdf_reader_make_streams_self_contained(resolved, src_data, src_len, cache, xref,
                                                    parser, crypt, visited, xref_count);
}

TspdfError tspdf_reader_make_streams_self_contained(TspdfObj *obj, const uint8_t *src_data, size_t src_len,
                                                    TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                                    TspdfCrypt *crypt, bool *visited, size_t xref_count) {
    if (!obj) return TSPDF_OK;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            return tspdf_reader_make_ref_self_contained(obj->ref.num, src_data, src_len, cache, xref,
                                                        parser, crypt, visited, xref_count);
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                TspdfError err = tspdf_reader_make_streams_self_contained(&obj->array.items[i], src_data, src_len,
                                                                          cache, xref, parser, crypt, visited, xref_count);
                if (err != TSPDF_OK) return err;
            }
            return TSPDF_OK;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                TspdfError err = tspdf_reader_make_streams_self_contained(obj->dict.entries[i].value, src_data, src_len,
                                                                          cache, xref, parser, crypt, visited, xref_count);
                if (err != TSPDF_OK) return err;
            }
            return TSPDF_OK;
        case TSPDF_OBJ_STREAM:
        {
            TspdfError err = copy_stream_source(obj, src_data, src_len, crypt, 0, 0);
            if (err != TSPDF_OK) return err;
            return tspdf_reader_make_streams_self_contained(obj->stream.dict, src_data, src_len,
                                                            cache, xref, parser, crypt, visited, xref_count);
        }
        default:
            return TSPDF_OK;
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
                TspdfError prep_err = tspdf_reader_make_ref_self_contained(page->obj_num, src->data, src->data_len,
                                                                           src->obj_cache, &src->xref, &parser,
                                                                           src->crypt, visited, src->xref.count);
                if (prep_err != TSPDF_OK) {
                    free(visited);
                    tspdf_arena_destroy(&doc->arena);
                    free(xref_offsets);
                    free(doc);
                    if (err) *err = prep_err;
                    return NULL;
                }
            }
            TspdfError prep_err = tspdf_reader_make_streams_self_contained(page->page_dict, src->data, src->data_len,
                                                                           src->obj_cache, &src->xref, &parser,
                                                                           src->crypt, visited, src->xref.count);
            if (prep_err != TSPDF_OK) {
                free(visited);
                tspdf_arena_destroy(&doc->arena);
                free(xref_offsets);
                free(doc);
                if (err) *err = prep_err;
                return NULL;
            }

            prep_err = materialize_inherited_page_attrs(src, &parser,
                                                        page->page_dict,
                                                        page,
                                                        &src->arena);
            if (prep_err != TSPDF_OK) {
                free(visited);
                tspdf_arena_destroy(&doc->arena);
                free(xref_offsets);
                free(doc);
                if (err) *err = prep_err;
                return NULL;
            }
        }

        // Resolve outline items and the AcroForm graph into the source cache
        // so the copy loop below picks them up alongside the pages.
        TspdfError trees_prep_err = tspdf_doctree_merge_prepare(src, &parser, visited);
        if (trees_prep_err != TSPDF_OK) {
            free(visited);
            tspdf_arena_destroy(&doc->arena);
            free(xref_offsets);
            free(doc);
            if (err) *err = trees_prep_err;
            return NULL;
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
            doc->pages.pages[pi].user_unit = src_page->user_unit > 0.0 ? src_page->user_unit : 1.0;
            doc->pages.pages[pi].rotate = src_page->rotate;
            pi++;
        }
    }

    // Rebuild the sources' outlines and AcroForm on the merged catalog.
    TspdfError trees_err = tspdf_doctree_merge_attach(doc, docs, count, xref_offsets);
    if (trees_err != TSPDF_OK) {
        tspdf_arena_destroy(&doc->arena);
        free(xref_offsets);
        free(doc->new_objs.objs);
        free(doc);
        if (err) *err = trees_err;
        return NULL;
    }

    // Union the sources' embedded-file attachments (first source wins on a
    // name collision).
    TspdfError attach_err = tspdf_attach_merge_attach(doc, docs, count);
    if (attach_err != TSPDF_OK) {
        tspdf_arena_destroy(&doc->arena);
        free(xref_offsets);
        free(doc->new_objs.objs);
        free(doc);
        if (err) *err = attach_err;
        return NULL;
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
    // Wipe the stack copy of the key material before returning.
    memset(&crypt, 0, sizeof(crypt));
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
