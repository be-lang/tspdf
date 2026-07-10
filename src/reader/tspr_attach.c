// Embedded file attachments (/Names /EmbeddedFiles).
//
// Reading walks the catalog name tree with the shared budget-guarded walker
// from tspr_doctree.c (hostile /Kids cycles terminate). Writing always
// rebuilds the tree as a single flat node — << /Names [k1 v1 k2 v2 ...] >>
// with lexicographically sorted keys — which is spec-legal at any size and
// never inherits a broken shape from the input.
//
// Preservation: extract rebuilds the catalog without /Names and merge only
// copies objects reachable from pages, so both would silently drop
// attachments (like outlines used to be dropped). The hooks at the bottom
// re-add every source attachment to the destination as a freshly built
// Filespec + /EmbeddedFile stream: bytes are decoded from the source
// (decrypting when needed) and re-deflated when that shrinks them, /Desc,
// /Subtype and /Params carry over. This one path handles buffer-backed,
// merged, encrypted and not-yet-saved sources alike.

#include "tspr_attach.h"
#include "tspr_doctree.h"
#include "../compress/deflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// --- small object builders (all allocations in the document's arena) ---

static TspdfObj *att_obj_new(TspdfArena *a, TspdfObjType type) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = type;
    return o;
}

static TspdfObj *att_obj_int(TspdfArena *a, int64_t v) {
    TspdfObj *o = att_obj_new(a, TSPDF_OBJ_INT);
    if (o) o->integer = v;
    return o;
}

static TspdfObj *att_obj_ref(TspdfArena *a, uint32_t num) {
    TspdfObj *o = att_obj_new(a, TSPDF_OBJ_REF);
    if (o) o->ref.num = num;
    return o;
}

// Name or string object with a NUL-terminated arena copy of `data`.
static TspdfObj *att_obj_text(TspdfArena *a, TspdfObjType type,
                              const uint8_t *data, size_t len) {
    TspdfObj *o = att_obj_new(a, type);
    if (!o) return NULL;
    o->string.data = (uint8_t *)tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    if (len > 0) memcpy(o->string.data, data, len);
    o->string.data[len] = '\0';
    o->string.len = len;
    return o;
}

static TspdfObj *att_obj_name(TspdfArena *a, const char *name) {
    return att_obj_text(a, TSPDF_OBJ_NAME, (const uint8_t *)name, strlen(name));
}

static char *att_arena_strndup(TspdfArena *a, const uint8_t *s, size_t len) {
    char *copy = (char *)tspdf_arena_alloc(a, len + 1);
    if (!copy) return NULL;
    if (len > 0) memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

// Remove `key` from a dict in place (entries arrays are arena-owned).
static void att_dict_remove(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            memmove(&dict->dict.entries[i], &dict->dict.entries[i + 1],
                    sizeof(TspdfDictEntry) * (dict->dict.count - i - 1));
            dict->dict.count--;
            return;
        }
    }
}

// True when any ref hides inside `obj` (deep-copied dicts keep refs, and a
// ref carried across documents would point at an arbitrary object in the
// destination's numbering). Bounded recursion: copies are trees.
static bool att_contains_ref(const TspdfObj *obj, int depth) {
    if (!obj || depth > 64) return depth > 64;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            return true;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                if (att_contains_ref(&obj->array.items[i], depth + 1)) return true;
            }
            return false;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                if (att_contains_ref(obj->dict.entries[i].value, depth + 1)) return true;
            }
            return false;
        case TSPDF_OBJ_STREAM:
            return true;  // streams cannot be carried inline either
        default:
            return false;
    }
}

// --- name-tree enumeration ---

typedef struct {
    const uint8_t *key;   // key bytes (owned by the source document)
    size_t key_len;
    TspdfObj *value;      // raw tree value (may be a ref), source numbering
} AttEntry;

typedef struct {
    AttEntry *items;
    size_t count;
    size_t cap;
    bool oom;
} AttList;

static bool att_list_find(const AttList *l, const uint8_t *key, size_t key_len,
                          size_t *out_idx) {
    for (size_t i = 0; i < l->count; i++) {
        if (l->items[i].key_len == key_len &&
            memcmp(l->items[i].key, key, key_len) == 0) {
            if (out_idx) *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool att_list_push(AttList *l, const uint8_t *key, size_t key_len,
                          TspdfObj *value) {
    if (l->count >= l->cap) {
        size_t cap = l->cap == 0 ? 8 : l->cap * 2;
        AttEntry *grown = (AttEntry *)realloc(l->items, cap * sizeof(AttEntry));
        if (!grown) {
            l->oom = true;
            return false;
        }
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->count].key = key;
    l->items[l->count].key_len = key_len;
    l->items[l->count].value = value;
    l->count++;
    return true;
}

// The catalog of a document (merged documents carry their catalog directly;
// opened documents also resolve the trailer /Root).
static TspdfObj *att_catalog(TspdfReader *doc, TspdfParser *parser) {
    if (doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT) return doc->catalog;
    if (!doc->xref.trailer) return NULL;
    TspdfObj *root = tspdf_doctree_resolve(doc, parser,
                                           tspdf_dict_get(doc->xref.trailer, "Root"));
    return root && root->type == TSPDF_OBJ_DICT ? root : NULL;
}

static TspdfObj *att_embedded_files_root(TspdfReader *doc, TspdfParser *parser) {
    TspdfObj *catalog = att_catalog(doc, parser);
    if (!catalog) return NULL;
    TspdfObj *names = tspdf_doctree_resolve(doc, parser,
                                            tspdf_dict_get(catalog, "Names"));
    if (!names || names->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *root = tspdf_doctree_resolve(doc, parser,
                                           tspdf_dict_get(names, "EmbeddedFiles"));
    return root && root->type == TSPDF_OBJ_DICT ? root : NULL;
}

static bool att_collect_visit(void *vctx, TspdfObj *key, TspdfObj *value) {
    AttList *list = (AttList *)vctx;
    if (key->type != TSPDF_OBJ_STRING) return true;
    // First-wins dedupe: a legitimate tree never repeats a key; a cyclic one
    // revisiting nodes must not inflate the list.
    if (att_list_find(list, key->string.data, key->string.len, NULL)) return true;
    return att_list_push(list, key->string.data, key->string.len, value);
}

// Gather all attachment entries of `doc` (first occurrence per key wins).
// The list is malloc'd (caller frees list->items); keys/values stay owned by
// the document.
static TspdfError att_collect(TspdfReader *doc, TspdfParser *parser, AttList *out) {
    memset(out, 0, sizeof(*out));
    TspdfObj *root = att_embedded_files_root(doc, parser);
    if (!root) return TSPDF_OK;
    // A legitimate walk visits at most one node per object; anything past
    // that is cyclic /Kids re-visiting.
    size_t budget = doc->xref.count + doc->new_objs.count + 16;
    tspdf_nametree_walk(doc, parser, root, 0, &budget, att_collect_visit, out);
    if (out->oom) {
        free(out->items);
        memset(out, 0, sizeof(*out));
        return TSPDF_ERR_ALLOC;
    }
    return TSPDF_OK;
}

// --- Filespec / embedded stream access ---

// Resolve a tree value to its Filespec dict (NULL when broken).
static TspdfObj *att_filespec(TspdfReader *doc, TspdfParser *parser, TspdfObj *value) {
    TspdfObj *fs = tspdf_doctree_resolve(doc, parser, value);
    return fs && fs->type == TSPDF_OBJ_DICT ? fs : NULL;
}

// Resolve the /EF stream of a Filespec; *out_num receives the stream's
// object number when it is indirect (needed for decryption), else 0.
static TspdfObj *att_ef_stream(TspdfReader *doc, TspdfParser *parser,
                               TspdfObj *fs, uint32_t *out_num) {
    *out_num = 0;
    if (!fs) return NULL;
    TspdfObj *ef = tspdf_doctree_resolve(doc, parser, tspdf_dict_get(fs, "EF"));
    if (!ef || ef->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *sval = tspdf_dict_get(ef, "F");
    if (!sval) sval = tspdf_dict_get(ef, "UF");
    if (sval && sval->type == TSPDF_OBJ_REF) *out_num = sval->ref.num;
    TspdfObj *s = tspdf_doctree_resolve(doc, parser, sval);
    return s && s->type == TSPDF_OBJ_STREAM ? s : NULL;
}

// Decoded bytes of an embedded-file stream: raw bytes from the backing
// buffer (decrypted when the document is encrypted) or from a
// self-contained copy, then run through the /Filter chain. *out is malloc'd.
static TspdfError att_stream_decoded(TspdfReader *doc, TspdfObj *s,
                                     uint32_t obj_num, uint8_t **out,
                                     size_t *out_len) {
    const uint8_t *raw;
    size_t raw_len;
    uint8_t *owned = NULL;

    if (s->stream.self_contained && s->stream.data != NULL) {
        raw = s->stream.data;
        raw_len = s->stream.len;
    } else {
        if (!doc->data || s->stream.raw_offset > doc->data_len ||
            s->stream.raw_len > doc->data_len - s->stream.raw_offset) {
            return TSPDF_ERR_PARSE;
        }
        raw = doc->data + s->stream.raw_offset;
        raw_len = s->stream.raw_len;
        if (doc->crypt && obj_num > 0 && obj_num < doc->xref.count && raw_len > 0) {
            size_t dec_len = 0;
            owned = tspdf_crypt_decrypt_stream(doc->crypt, obj_num,
                                               doc->xref.entries[obj_num].gen,
                                               raw, raw_len, &dec_len);
            if (!owned) return TSPDF_ERR_PARSE;
            raw = owned;
            raw_len = dec_len;
        }
    }

    TspdfError err = tspdf_stream_decode(s->stream.dict, raw, raw_len, out, out_len);
    free(owned);
    return err;
}

// --- writing the flat tree ---

static int att_entry_cmp(const void *va, const void *vb) {
    const AttEntry *a = (const AttEntry *)va;
    const AttEntry *b = (const AttEntry *)vb;
    size_t min = a->key_len < b->key_len ? a->key_len : b->key_len;
    int c = memcmp(a->key, b->key, min);
    if (c != 0) return c;
    if (a->key_len == b->key_len) return 0;
    return a->key_len < b->key_len ? -1 : 1;
}

// Give `doc` a private, mutable catalog dict. Opened documents already carry
// one; documents built by delete/rotate/reorder resolve theirs through the
// shared trailer, so mutating that in place would leak the change into the
// source — copy it instead.
static TspdfError att_ensure_catalog(TspdfReader *doc, TspdfParser *parser) {
    if (doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT) return TSPDF_OK;
    TspdfObj *src = att_catalog(doc, parser);
    TspdfObj *copy = NULL;
    if (src) {
        copy = tspdf_obj_deep_copy(src, &doc->arena);
    } else {
        copy = att_obj_new(&doc->arena, TSPDF_OBJ_DICT);
    }
    if (!copy || copy->type != TSPDF_OBJ_DICT) return TSPDF_ERR_ALLOC;
    doc->catalog = copy;
    return TSPDF_OK;
}

// Rebuild catalog /Names -> /EmbeddedFiles as a flat sorted node holding
// `list` (which may be empty: then /EmbeddedFiles — and /Names, when nothing
// else lives there — disappear). Keys are copied into the document's arena;
// entry values must already be valid in the document's numbering.
static TspdfError att_write_tree(TspdfReader *doc, TspdfParser *parser,
                                 AttList *list) {
    TspdfError err = att_ensure_catalog(doc, parser);
    if (err != TSPDF_OK) return err;

    // Fresh /Names dict carrying over everything but /EmbeddedFiles. Built
    // fresh (not mutated in place) because the existing dict may be an
    // indirect object shared with other references.
    TspdfObj *old_names = tspdf_doctree_resolve(doc, parser,
                                                tspdf_dict_get(doc->catalog, "Names"));
    TspdfObj *names = att_obj_new(&doc->arena, TSPDF_OBJ_DICT);
    if (!names) return TSPDF_ERR_ALLOC;
    if (old_names && old_names->type == TSPDF_OBJ_DICT) {
        for (size_t i = 0; i < old_names->dict.count; i++) {
            const char *key = old_names->dict.entries[i].key;
            if (strcmp(key, "EmbeddedFiles") == 0) continue;
            err = tspdf_obj_dict_put(names, key, old_names->dict.entries[i].value,
                                     &doc->arena);
            if (err != TSPDF_OK) return err;
        }
    }

    if (list->count > 0) {
        qsort(list->items, list->count, sizeof(AttEntry), att_entry_cmp);

        TspdfObj *arr = att_obj_new(&doc->arena, TSPDF_OBJ_ARRAY);
        if (!arr) return TSPDF_ERR_ALLOC;
        arr->array.count = list->count * 2;
        arr->array.items = (TspdfObj *)tspdf_arena_alloc(&doc->arena,
            sizeof(TspdfObj) * arr->array.count);
        if (!arr->array.items) return TSPDF_ERR_ALLOC;
        for (size_t i = 0; i < list->count; i++) {
            TspdfObj *key = att_obj_text(&doc->arena, TSPDF_OBJ_STRING,
                                         list->items[i].key, list->items[i].key_len);
            if (!key || !list->items[i].value) return TSPDF_ERR_ALLOC;
            arr->array.items[2 * i] = *key;
            arr->array.items[2 * i + 1] = *list->items[i].value;
        }

        TspdfObj *node = att_obj_new(&doc->arena, TSPDF_OBJ_DICT);
        if (!node) return TSPDF_ERR_ALLOC;
        err = tspdf_obj_dict_put(node, "Names", arr, &doc->arena);
        if (err != TSPDF_OK) return err;
        uint32_t node_num = tspdf_register_new_obj(doc, node);
        if (node_num == 0) return TSPDF_ERR_ALLOC;
        TspdfObj *node_ref = att_obj_ref(&doc->arena, node_num);
        if (!node_ref) return TSPDF_ERR_ALLOC;
        err = tspdf_obj_dict_put(names, "EmbeddedFiles", node_ref, &doc->arena);
        if (err != TSPDF_OK) return err;
    }

    if (names->dict.count == 0) {
        att_dict_remove(doc->catalog, "Names");
    } else {
        err = tspdf_obj_dict_put(doc->catalog, "Names", names, &doc->arena);
        if (err != TSPDF_OK) return err;
    }

    doc->modified = true;
    return TSPDF_OK;
}

// --- adding ---

// Build + register an /EmbeddedFile stream for `data` (decoded bytes),
// deflating when that shrinks it. `subtype` (a MIME type) and `params_src`
// (a /Params dict to carry over, deep-copied; refs inside disqualify it) are
// optional. Returns the object number, 0 on allocation failure.
static uint32_t att_make_ef_stream(TspdfReader *doc, const uint8_t *data,
                                   size_t len, const uint8_t *subtype,
                                   size_t subtype_len, TspdfObj *params_src) {
    TspdfArena *a = &doc->arena;

    const uint8_t *stored = data;
    size_t stored_len = len;
    bool flate = false;
    uint8_t *comp = NULL;
    if (len > 0) {
        size_t comp_len = 0;
        comp = deflate_compress(data, len, &comp_len);
        if (comp && comp_len < len) {
            stored = comp;
            stored_len = comp_len;
            flate = true;
        }
    }

    uint8_t *bytes = (uint8_t *)tspdf_arena_alloc(a, stored_len ? stored_len : 1);
    if (!bytes) {
        free(comp);
        return 0;
    }
    if (stored_len > 0) memcpy(bytes, stored, stored_len);
    free(comp);

    TspdfObj *dict = att_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *type = att_obj_name(a, "EmbeddedFile");
    TspdfObj *length = att_obj_int(a, (int64_t)stored_len);
    if (!dict || !type || !length) return 0;
    if (tspdf_obj_dict_put(dict, "Type", type, a) != TSPDF_OK) return 0;
    if (subtype && subtype_len > 0) {
        TspdfObj *st = att_obj_text(a, TSPDF_OBJ_NAME, subtype, subtype_len);
        if (!st || tspdf_obj_dict_put(dict, "Subtype", st, a) != TSPDF_OK) return 0;
    }
    if (flate) {
        TspdfObj *filter = att_obj_name(a, "FlateDecode");
        if (!filter || tspdf_obj_dict_put(dict, "Filter", filter, a) != TSPDF_OK) {
            return 0;
        }
    }
    if (tspdf_obj_dict_put(dict, "Length", length, a) != TSPDF_OK) return 0;

    TspdfObj *params = NULL;
    if (params_src && params_src->type == TSPDF_OBJ_DICT &&
        !att_contains_ref(params_src, 0)) {
        params = tspdf_obj_deep_copy(params_src, a);
    }
    if (!params) {
        params = att_obj_new(a, TSPDF_OBJ_DICT);
        if (!params) return 0;
        // Fresh attachments get a modification date; carried-over /Params
        // dicts keep whatever dates the source recorded.
        char mod_date[80];
        time_t now = time(NULL);
        struct tm *tm_info = gmtime(&now);
        if (tm_info) {
            snprintf(mod_date, sizeof(mod_date), "D:%04d%02d%02d%02d%02d%02dZ",
                     tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                     tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
            TspdfObj *md = att_obj_text(a, TSPDF_OBJ_STRING,
                                        (const uint8_t *)mod_date, strlen(mod_date));
            if (!md || tspdf_obj_dict_put(params, "ModDate", md, a) != TSPDF_OK) {
                return 0;
            }
        }
    }
    TspdfObj *size = att_obj_int(a, (int64_t)len);
    if (!size || tspdf_obj_dict_put(params, "Size", size, a) != TSPDF_OK) return 0;
    if (tspdf_obj_dict_put(dict, "Params", params, a) != TSPDF_OK) return 0;

    TspdfObj *stream = att_obj_new(a, TSPDF_OBJ_STREAM);
    if (!stream) return 0;
    stream->stream.dict = dict;
    stream->stream.data = bytes;          // arena-owned: destroy never frees it
    stream->stream.len = stored_len;
    stream->stream.raw_offset = 0;
    stream->stream.raw_len = stored_len;
    stream->stream.self_contained = true;

    return tspdf_register_new_obj(doc, stream);
}

// Build + register the Filespec for an embedded stream. Returns the object
// number, 0 on allocation failure.
static uint32_t att_make_filespec(TspdfReader *doc, const uint8_t *name,
                                  size_t name_len, const uint8_t *desc,
                                  size_t desc_len, uint32_t ef_num) {
    TspdfArena *a = &doc->arena;
    TspdfObj *fs = att_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *type = att_obj_name(a, "Filespec");
    TspdfObj *f = att_obj_text(a, TSPDF_OBJ_STRING, name, name_len);
    TspdfObj *uf = att_obj_text(a, TSPDF_OBJ_STRING, name, name_len);
    TspdfObj *ef = att_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *ef_ref = att_obj_ref(a, ef_num);
    if (!fs || !type || !f || !uf || !ef || !ef_ref) return 0;
    if (tspdf_obj_dict_put(fs, "Type", type, a) != TSPDF_OK) return 0;
    if (tspdf_obj_dict_put(fs, "F", f, a) != TSPDF_OK) return 0;
    if (tspdf_obj_dict_put(fs, "UF", uf, a) != TSPDF_OK) return 0;
    if (desc && desc_len > 0) {
        TspdfObj *d = att_obj_text(a, TSPDF_OBJ_STRING, desc, desc_len);
        if (!d || tspdf_obj_dict_put(fs, "Desc", d, a) != TSPDF_OK) return 0;
    }
    if (tspdf_obj_dict_put(ef, "F", ef_ref, a) != TSPDF_OK) return 0;
    if (tspdf_obj_dict_put(fs, "EF", ef, a) != TSPDF_OK) return 0;
    return tspdf_register_new_obj(doc, fs);
}

// Shared add: create the objects, drop any same-named entry (add = replace),
// rebuild the flat tree. subtype/params_src are the carry-over channel used
// by the extract/merge preservation hooks.
static TspdfError att_add_internal(TspdfReader *doc,
                                   const uint8_t *name, size_t name_len,
                                   const uint8_t *data, size_t len,
                                   const uint8_t *desc, size_t desc_len,
                                   const uint8_t *subtype, size_t subtype_len,
                                   TspdfObj *params_src) {
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    AttList list;
    TspdfError err = att_collect(doc, &parser, &list);
    if (err != TSPDF_OK) return err;

    uint32_t ef_num = att_make_ef_stream(doc, data, len, subtype, subtype_len,
                                         params_src);
    uint32_t fs_num = ef_num ? att_make_filespec(doc, name, name_len,
                                                 desc, desc_len, ef_num) : 0;
    TspdfObj *fs_ref = fs_num ? att_obj_ref(&doc->arena, fs_num) : NULL;
    if (!fs_ref) {
        free(list.items);
        return TSPDF_ERR_ALLOC;
    }

    size_t existing;
    if (att_list_find(&list, name, name_len, &existing)) {
        list.items[existing].value = fs_ref;    // replace in place
    } else if (!att_list_push(&list, name, name_len, fs_ref)) {
        free(list.items);
        return TSPDF_ERR_ALLOC;
    }

    err = att_write_tree(doc, &parser, &list);
    free(list.items);
    return err;
}

// --- public API ---

TspdfError tspdf_reader_attachments(TspdfReader *doc, TspdfAttachmentInfo **out,
                                    size_t *count) {
    if (!doc || !out || !count) return TSPDF_ERR_INVALID_ARG;
    *out = NULL;
    *count = 0;

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    AttList list;
    TspdfError err = att_collect(doc, &parser, &list);
    if (err != TSPDF_OK) return err;
    if (list.count == 0) {
        free(list.items);
        return TSPDF_OK;
    }

    TspdfAttachmentInfo *infos = (TspdfAttachmentInfo *)tspdf_arena_alloc_zero(
        &doc->arena, sizeof(TspdfAttachmentInfo) * list.count);
    if (!infos) {
        free(list.items);
        return TSPDF_ERR_ALLOC;
    }

    size_t n = 0;
    for (size_t i = 0; i < list.count; i++) {
        TspdfObj *fs = att_filespec(doc, &parser, list.items[i].value);
        uint32_t snum = 0;
        TspdfObj *stream = att_ef_stream(doc, &parser, fs, &snum);
        if (!stream) continue;  // broken Filespec: skip

        infos[n].name = att_arena_strndup(&doc->arena, list.items[i].key,
                                          list.items[i].key_len);
        if (!infos[n].name) {
            free(list.items);
            return TSPDF_ERR_ALLOC;
        }

        TspdfObj *desc = tspdf_dict_get(fs, "Desc");
        if (desc && desc->type == TSPDF_OBJ_STRING) {
            infos[n].desc = att_arena_strndup(&doc->arena, desc->string.data,
                                              desc->string.len);
        }
        TspdfObj *st = tspdf_dict_get(stream->stream.dict, "Subtype");
        if (st && st->type == TSPDF_OBJ_NAME) {
            infos[n].mime = att_arena_strndup(&doc->arena, st->string.data,
                                              st->string.len);
        }

        TspdfObj *params = tspdf_doctree_resolve(doc, &parser,
                                                 tspdf_dict_get(stream->stream.dict,
                                                                "Params"));
        TspdfObj *size = params ? tspdf_dict_get(params, "Size") : NULL;
        if (size && size->type == TSPDF_OBJ_INT && size->integer >= 0) {
            infos[n].size = (size_t)size->integer;
        } else {
            uint8_t *bytes = NULL;
            size_t blen = 0;
            if (att_stream_decoded(doc, stream, snum, &bytes, &blen) == TSPDF_OK) {
                infos[n].size = blen;
                free(bytes);
            }
        }
        n++;
    }
    free(list.items);

    *out = n > 0 ? infos : NULL;
    *count = n;
    return TSPDF_OK;
}

TspdfError tspdf_reader_attachment_get(TspdfReader *doc, const char *name,
                                       uint8_t **out, size_t *out_len) {
    if (!doc || !name || !out || !out_len) return TSPDF_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    AttList list;
    TspdfError err = att_collect(doc, &parser, &list);
    if (err != TSPDF_OK) return err;

    size_t idx;
    if (!att_list_find(&list, (const uint8_t *)name, strlen(name), &idx)) {
        free(list.items);
        return TSPDF_ERR_NOT_FOUND;
    }

    TspdfObj *fs = att_filespec(doc, &parser, list.items[idx].value);
    uint32_t snum = 0;
    TspdfObj *stream = att_ef_stream(doc, &parser, fs, &snum);
    free(list.items);
    if (!stream) return TSPDF_ERR_PARSE;

    return att_stream_decoded(doc, stream, snum, out, out_len);
}

TspdfError tspdf_reader_attachment_add(TspdfReader *doc, const char *name,
                                       const uint8_t *data, size_t len,
                                       const char *desc) {
    if (!doc || !name || name[0] == '\0' || (!data && len > 0)) {
        return TSPDF_ERR_INVALID_ARG;
    }
    return att_add_internal(doc, (const uint8_t *)name, strlen(name),
                            data, len,
                            (const uint8_t *)desc, desc ? strlen(desc) : 0,
                            NULL, 0, NULL);
}

TspdfError tspdf_reader_attachment_remove(TspdfReader *doc, const char *name) {
    if (!doc || !name) return TSPDF_ERR_INVALID_ARG;

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    AttList list;
    TspdfError err = att_collect(doc, &parser, &list);
    if (err != TSPDF_OK) return err;

    size_t idx;
    if (!att_list_find(&list, (const uint8_t *)name, strlen(name), &idx)) {
        free(list.items);
        return TSPDF_ERR_NOT_FOUND;
    }
    memmove(&list.items[idx], &list.items[idx + 1],
            sizeof(AttEntry) * (list.count - idx - 1));
    list.count--;

    err = att_write_tree(doc, &parser, &list);
    free(list.items);
    return err;
}

// --- preservation across extract and merge ---

// Re-add one source attachment entry to `dst`: decode the bytes in the
// source (decrypting when needed) and rebuild Filespec + stream fresh in the
// destination, carrying /Desc, /Subtype and /Params over. Broken source
// entries are skipped rather than failing the whole operation.
static TspdfError att_copy_entry(TspdfReader *src, TspdfParser *sp,
                                 const AttEntry *e, TspdfReader *dst) {
    TspdfObj *fs = att_filespec(src, sp, e->value);
    uint32_t snum = 0;
    TspdfObj *stream = att_ef_stream(src, sp, fs, &snum);
    if (!stream) return TSPDF_OK;

    uint8_t *bytes = NULL;
    size_t blen = 0;
    TspdfError err = att_stream_decoded(src, stream, snum, &bytes, &blen);
    if (err == TSPDF_ERR_ALLOC) return err;
    if (err != TSPDF_OK) return TSPDF_OK;  // undecodable filter chain: skip

    const uint8_t *desc = NULL;
    size_t desc_len = 0;
    TspdfObj *d = tspdf_dict_get(fs, "Desc");
    if (d && d->type == TSPDF_OBJ_STRING) {
        desc = d->string.data;
        desc_len = d->string.len;
    }
    const uint8_t *subtype = NULL;
    size_t subtype_len = 0;
    TspdfObj *st = tspdf_dict_get(stream->stream.dict, "Subtype");
    if (st && st->type == TSPDF_OBJ_NAME) {
        subtype = st->string.data;
        subtype_len = st->string.len;
    }
    TspdfObj *params = tspdf_doctree_resolve(src, sp,
                                             tspdf_dict_get(stream->stream.dict,
                                                            "Params"));

    err = att_add_internal(dst, e->key, e->key_len, bytes, blen,
                           desc, desc_len, subtype, subtype_len, params);
    free(bytes);
    return err;
}

TspdfError tspdf_attach_extract_attach(TspdfReader *src, TspdfReader *dst) {
    TspdfParser sp;
    tspdf_parser_init(&sp, src->data, src->data_len, &src->arena);

    AttList list;
    TspdfError err = att_collect(src, &sp, &list);
    if (err != TSPDF_OK) return err;

    for (size_t i = 0; i < list.count && err == TSPDF_OK; i++) {
        err = att_copy_entry(src, &sp, &list.items[i], dst);
    }
    free(list.items);
    return err;
}

TspdfError tspdf_attach_merge_attach(TspdfReader *merged, TspdfReader **docs,
                                     size_t count) {
    // Names already added to `merged` (from an earlier source) win; check
    // against the merged document's own tree so the rule holds across all
    // sources without a separate bookkeeping list.
    TspdfError err = TSPDF_OK;
    for (size_t d = 0; d < count && err == TSPDF_OK; d++) {
        TspdfReader *src = docs[d];
        TspdfParser sp;
        tspdf_parser_init(&sp, src->data, src->data_len, &src->arena);

        AttList list;
        err = att_collect(src, &sp, &list);
        if (err != TSPDF_OK) break;

        TspdfParser mp;
        tspdf_parser_init(&mp, merged->data, merged->data_len, &merged->arena);
        AttList have;
        err = att_collect(merged, &mp, &have);
        if (err != TSPDF_OK) {
            free(list.items);
            break;
        }

        for (size_t i = 0; i < list.count && err == TSPDF_OK; i++) {
            if (att_list_find(&have, list.items[i].key, list.items[i].key_len,
                              NULL)) {
                continue;  // first source wins
            }
            err = att_copy_entry(src, &sp, &list.items[i], merged);
        }
        free(have.items);
        free(list.items);
    }
    return err;
}
