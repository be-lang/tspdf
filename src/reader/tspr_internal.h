#ifndef TSPDF_READER_INTERNAL_H
#define TSPDF_READER_INTERNAL_H

#include "tspr.h"
#include "../util/arena.h"
#include "../crypto/aes.h"

// --- Parser ---

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    TspdfArena *arena;
    TspdfError error;
    int depth;              // current array/dict nesting depth (recursion guard)
} TspdfParser;

// Maximum array/dict nesting depth. A hostile PDF with thousands of nested
// [[[...]]] or <<...>> would otherwise recurse until the C stack overflows
// (verified DoS). 256 comfortably exceeds real-world nesting.
#define TSPDF_MAX_PARSE_DEPTH 256

void tspdf_parser_init(TspdfParser *p, const uint8_t *data, size_t len, TspdfArena *arena);
void tspdf_skip_whitespace(TspdfParser *p);
TspdfObj *tspdf_parse_obj(TspdfParser *p);
TspdfObj *tspdf_parse_indirect_obj(TspdfParser *p, uint32_t *out_num, uint16_t *out_gen);

// --- Encryption ---

typedef struct {
    uint8_t file_key[32];
    int key_len;            // 5, 16, or 32 bytes
    int version;            // /V value (1-5)
    int revision;           // /R value (2-6)
    bool use_aes;           // true for AES, false for RC4
    bool identity_stm;      // V=4 StmF == Identity: streams are not encrypted
    bool identity_str;      // V=4 StrF == Identity: strings are not encrypted
    bool encrypt_metadata;  // /EncryptMetadata (default true); Algorithm 2 step (f)
    uint32_t permissions;   // /P value
    uint8_t *file_id;      // first file ID string (malloc'd)
    size_t file_id_len;
    // Encryption output fields (used by encrypt_init for serialization)
    uint8_t O[48];
    uint8_t U[48];
    uint8_t OE[32];
    uint8_t UE[32];
    uint8_t Perms[16];
    size_t O_len;           // 32 for V<=4, 48 for V=5
    size_t U_len;           // 32 for V<=4, 48 for V=5
    // V=5 only: the file key IS the object key (no per-object derivation), so
    // one AES-256 schedule serves every string/stream. Built lazily on first
    // use by tspr_crypt.c; V<=4 derives a per-object key and cannot cache.
    Aes aes_v5;
    bool aes_v5_ready;
    // Source-document /Encrypt preservation (set for crypts created by
    // tspdf_crypt_init when opening an encrypted file, NULL for crypts made
    // by tspdf_crypt_encrypt_init). The dict lives in the owning document's
    // arena; encrypted saves copy it verbatim so the original passwords (and
    // permissions) keep working — the recovered file key is reused, since
    // the "other" password can never be re-derived. src_encrypt_num is the
    // object number of the source /Encrypt dict (0 when it was inline) so
    // the serializer can skip that now-superseded object.
    TspdfObj *src_encrypt_dict;
    uint32_t src_encrypt_num;
} TspdfCrypt;

// --- Metadata ---

typedef struct {
    char *title, *author, *subject, *keywords, *creator, *producer;
    bool title_set, author_set, subject_set, keywords_set, creator_set, producer_set;
} TspdfReaderMetadata;

// --- Xref ---

typedef struct {
    size_t offset;
    uint16_t gen;
    bool in_use;
    bool compressed;
    bool seen;           // true once any xref section has set this entry
    uint32_t stream_obj;
    size_t stream_idx;
} TspdfReaderXrefEntry;

typedef struct {
    TspdfReaderXrefEntry *entries;
    size_t count;
    TspdfObj *trailer;
} TspdfReaderXref;

TspdfError tspdf_xref_parse(TspdfParser *p, TspdfReaderXref *xref);
// Last-resort rebuild: scan the whole buffer for "N G obj" markers and build a
// synthetic xref + trailer. Used by the document layer when a parsed xref does
// not yield a usable catalog (e.g. a file behind a large transfer prefix whose
// offsets are all shifted). Clears any prior entries/trailer before scanning.
TspdfError tspdf_xref_reconstruct_by_scan(TspdfParser *p, TspdfReaderXref *xref);
TspdfObj *tspdf_xref_resolve(TspdfReaderXref *xref, TspdfParser *p, uint32_t obj_num, TspdfObj **cache, TspdfCrypt *crypt);

// --- Pages ---

typedef struct {
    TspdfReaderPage *pages;
    size_t count;
} TspdfReaderPageList;

TspdfError tspdf_pages_load(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                          TspdfObj *catalog, TspdfReaderPageList *out, TspdfCrypt *crypt);

// --- Serialization ---

TspdfError tspdf_serialize(TspdfReader *doc, uint8_t **out_buf, size_t *out_len);
TspdfError tspdf_serialize_with_options(TspdfReader *doc, uint8_t **out_buf, size_t *out_len,
                                      const TspdfSaveOptions *opts);

// --- New object list (for Phase 3 content overlay / annotations) ---

typedef struct {
    TspdfObj **objs;
    size_t count;
    size_t capacity;
} TspdfNewObjList;

// --- Document (full definition) ---

struct TspdfReader {
    const uint8_t *data;
    size_t data_len;
    bool owns_data;
    TspdfArena arena;
    TspdfReaderXref xref;
    TspdfReaderPageList pages;
    TspdfObj *catalog;
    TspdfObj **obj_cache;
    char pdf_version[8];    // e.g. "1.7"
    TspdfCrypt *crypt;           // non-NULL if encrypted (malloc'd)
    TspdfReaderMetadata *metadata;     // non-NULL if metadata modified (malloc'd)
    TspdfNewObjList new_objs;    // objects created by Phase 3 APIs
    bool modified;              // set by manipulation/annotation/metadata functions
};

// --- Crypt ---

TspdfError tspdf_crypt_init(TspdfCrypt *crypt, TspdfObj *encrypt_dict,
                          TspdfObj *trailer, const char *password);
TspdfError tspdf_crypt_decrypt_string(TspdfCrypt *crypt, uint32_t obj_num,
                                     uint16_t gen, uint8_t *data, size_t *len);
uint8_t *tspdf_crypt_decrypt_stream(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
TspdfError tspdf_crypt_encrypt_init(TspdfCrypt *crypt, const char *user_pass,
                                   const char *owner_pass, uint32_t permissions,
                                   int key_bits);
uint8_t *tspdf_crypt_encrypt_string(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
uint8_t *tspdf_crypt_encrypt_stream(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
// Clone a reader's crypt into a derived document so its save preserves the
// source encryption (malloc'd; freed by tspdf_reader_destroy). The clone's
// src_encrypt_dict points into the source's arena: valid only under the
// derived-document rule that the source outlives it until it is saved.
TspdfCrypt *tspdf_crypt_clone(const TspdfCrypt *src);
void tspdf_random_bytes(uint8_t *buf, size_t len);
TspdfError tspdf_serialize_encrypted(TspdfReader *doc, TspdfCrypt *crypt,
                                    uint8_t **out_buf, size_t *out_len);

// --- Resource merging ---

typedef struct {
    char *old_name;     // arena-allocated
    char *new_name;     // arena-allocated
} TspdfResourceRename;

typedef struct {
    TspdfResourceRename *renames;    // arena-allocated array
    size_t count;
    size_t capacity;
} TspdfRenameMap;

TspdfError tspdf_resources_merge(TspdfObj *page_dict, TspdfObj *new_resources,
                                TspdfArena *arena, TspdfRenameMap *renames);
uint8_t *tspdf_content_rewrite(const uint8_t *stream, size_t len,
                               const TspdfRenameMap *renames, size_t *out_len,
                               TspdfArena *arena);

// --- Stream filter decoding ---

// Run a stream's /Filter chain over raw (already decrypted) bytes. Returns a
// malloc'd buffer the caller frees. Defined in tspr_xref.c, used by the text
// extractor.
TspdfError tspdf_stream_decode(TspdfObj *stream_dict, const uint8_t *raw_data,
                               size_t raw_len, uint8_t **out_data, size_t *out_len);

// --- New object registration ---

uint32_t tspdf_register_new_obj(TspdfReader *doc, TspdfObj *obj);

// --- Helpers ---

// Dict lookup helper: returns NULL if not found
TspdfObj *tspdf_dict_get(TspdfObj *dict, const char *key);

// Deep-copy an object into a target arena
TspdfObj *tspdf_obj_deep_copy(TspdfObj *obj, TspdfArena *dst);

// Bracket a page's /Contents with new streams: [prefix, ...old, suffix].
// Either side may be NULL to skip it. Used by page scaling to wrap the
// existing content in a "q <cm> ... Q" transform. Defined in tspr_content.c.
TspdfError tspdf_page_wrap_content(TspdfReader *doc, size_t page_index,
                                   const uint8_t *prefix, size_t prefix_len,
                                   const uint8_t *suffix, size_t suffix_len);

#endif
