#ifndef TSPDF_READER_H
#define TSPDF_READER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../tspdf_error.h"

// --- PDF object types ---

typedef enum {
    TSPDF_OBJ_NULL,
    TSPDF_OBJ_BOOL,
    TSPDF_OBJ_INT,
    TSPDF_OBJ_REAL,
    TSPDF_OBJ_STRING,
    TSPDF_OBJ_NAME,
    TSPDF_OBJ_ARRAY,
    TSPDF_OBJ_DICT,
    TSPDF_OBJ_STREAM,
    TSPDF_OBJ_REF,
} TspdfObjType;

typedef struct TspdfDictEntry TspdfDictEntry;
typedef struct TspdfObj TspdfObj;

struct TspdfDictEntry {
    char *key;      // name without leading '/', arena-allocated
    TspdfObj *value;  // arena-allocated
};

struct TspdfObj {
    TspdfObjType type;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct { uint8_t *data; size_t len; } string;   // also used for TSPDF_OBJ_NAME
        struct { TspdfObj *items; size_t count; } array;
        struct { TspdfDictEntry *entries; size_t count; } dict;
        struct {
            TspdfObj *dict;          // always TSPDF_OBJ_DICT
            uint8_t *data;          // decompressed data (NULL until accessed, malloc'd)
            size_t len;             // decompressed length
            size_t raw_offset;      // offset in source buffer
            size_t raw_len;         // raw length in source buffer
            bool self_contained;    // if true, write stream.data directly (len bytes)
        } stream;
        struct { uint32_t num; uint16_t gen; } ref;
    };
};

// --- Page ---

typedef struct {
    uint32_t obj_num;
    TspdfObj *page_dict;
    double media_box[4];    // [x0, y0, x1, y1]
    int rotate;             // 0, 90, 180, 270
} TspdfReaderPage;

// --- Document (opaque) ---

typedef struct TspdfReader TspdfReader;

// --- API ---

// Open/close
TspdfReader *tspdf_reader_open(const uint8_t *data, size_t len, TspdfError *err);
TspdfReader *tspdf_reader_open_file(const char *path, TspdfError *err);
void tspdf_reader_destroy(TspdfReader *doc);

// Query
size_t tspdf_reader_page_count(const TspdfReader *doc);
TspdfReaderPage *tspdf_reader_get_page(const TspdfReader *doc, size_t index);

// Manipulate (returns new document, source unchanged).
// The source document must outlive the returned document unless saved first,
// because the returned document references the source's stream data.
// Merge copies stream data, so merged documents are self-contained.
TspdfReader *tspdf_reader_extract(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err);
TspdfReader *tspdf_reader_delete(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err);
TspdfReader *tspdf_reader_rotate(TspdfReader *doc, const size_t *pages, size_t count, int angle, TspdfError *err);
TspdfReader *tspdf_reader_reorder(TspdfReader *doc, const size_t *order, size_t count, TspdfError *err);
TspdfReader *tspdf_reader_merge(TspdfReader **docs, size_t count, TspdfError *err);

// Save
TspdfError tspdf_reader_save(TspdfReader *doc, const char *path);
TspdfError tspdf_reader_save_to_memory(TspdfReader *doc, uint8_t **out, size_t *out_len);

// Open encrypted PDF with password
TspdfReader *tspdf_reader_open_with_password(const uint8_t *data, size_t len,
                                                const char *password, TspdfError *err);
TspdfReader *tspdf_reader_open_file_with_password(const char *path,
                                                     const char *password, TspdfError *err);

// Save with encryption (key_bits: 128=AES-128/V4R4, 256=AES-256/V5R6)
TspdfError tspdf_reader_save_encrypted(TspdfReader *doc, const char *path,
                                        const char *user_pass, const char *owner_pass,
                                        uint32_t permissions, int key_bits);
TspdfError tspdf_reader_save_to_memory_encrypted(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                  const char *user_pass, const char *owner_pass,
                                                  uint32_t permissions, int key_bits);

// Metadata getters (return NULL if not present)
const char *tspdf_reader_get_title(const TspdfReader *doc);
const char *tspdf_reader_get_author(const TspdfReader *doc);
const char *tspdf_reader_get_subject(const TspdfReader *doc);
const char *tspdf_reader_get_keywords(const TspdfReader *doc);
const char *tspdf_reader_get_creator(const TspdfReader *doc);
const char *tspdf_reader_get_producer(const TspdfReader *doc);
const char *tspdf_reader_get_creation_date(const TspdfReader *doc);
const char *tspdf_reader_get_mod_date(const TspdfReader *doc);

// Metadata setters (pass NULL to remove a field)
void tspdf_reader_set_title(TspdfReader *doc, const char *value);
void tspdf_reader_set_author(TspdfReader *doc, const char *value);
void tspdf_reader_set_subject(TspdfReader *doc, const char *value);
void tspdf_reader_set_keywords(TspdfReader *doc, const char *value);
void tspdf_reader_set_creator(TspdfReader *doc, const char *value);
void tspdf_reader_set_producer(TspdfReader *doc, const char *value);

// --- Save options ---

typedef struct {
    bool preserve_object_ids;   // Keep original object numbers (enables raw byte copy, faster)
    bool strip_unused_objects;  // Only include objects reachable from page tree
    bool use_xref_stream;      // Use compact xref stream (PDF 1.5+) instead of classic table
    bool recompress_streams;   // Decompress + recompress all FlateDecode streams
    bool strip_metadata;       // Remove Info dict and XMP metadata
    bool update_producer;      // Set /Producer to "tspdf" (default true)
} TspdfSaveOptions;

TspdfSaveOptions tspdf_save_options_default(void);
TspdfError tspdf_reader_save_with_options(TspdfReader *doc, const char *path,
                                          const TspdfSaveOptions *opts);
TspdfError tspdf_reader_save_to_memory_with_options(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                     const TspdfSaveOptions *opts);

// --- Annotations ---

TspdfError tspdf_page_add_link(TspdfReader *doc, size_t page_index,
                              double x, double y, double w, double h,
                              const char *url);
TspdfError tspdf_page_add_link_to_page(TspdfReader *doc, size_t page_index,
                                      double x, double y, double w, double h,
                                      size_t target_page);
TspdfError tspdf_page_add_text_note(TspdfReader *doc, size_t page_index,
                                   double x, double y,
                                   const char *title, const char *contents);
TspdfError tspdf_page_add_stamp(TspdfReader *doc, size_t page_index,
                               double x, double y, double w, double h,
                               const char *stamp_name);

#endif
