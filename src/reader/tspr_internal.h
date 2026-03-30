#ifndef TSPDF_READER_INTERNAL_H
#define TSPDF_READER_INTERNAL_H

#include "tspr.h"
#include "../util/arena.h"

// --- Parser ---

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    TspdfArena *arena;
    TspdfError error;
} TspdfParser;

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
    uint16_t stream_idx;
} TspdfReaderXrefEntry;

typedef struct {
    TspdfReaderXrefEntry *entries;
    size_t count;
    TspdfObj *trailer;
} TspdfReaderXref;

TspdfError tspdf_xref_parse(TspdfParser *p, TspdfReaderXref *xref);
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

// --- New object registration ---

uint32_t tspdf_register_new_obj(TspdfReader *doc, TspdfObj *obj);

// --- Helpers ---

// Dict lookup helper: returns NULL if not found
TspdfObj *tspdf_dict_get(TspdfObj *dict, const char *key);

// Deep-copy an object into a target arena
TspdfObj *tspdf_obj_deep_copy(TspdfObj *obj, TspdfArena *dst);

#endif
