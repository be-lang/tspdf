#ifndef TSPDF_RAW_WRITER_H
#define TSPDF_RAW_WRITER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include "../util/buffer.h"

// Maximum number of indirect objects in a PDF
#define TSPDF_MAX_OBJECTS 4096

// An indirect object reference: "N 0 R"
typedef struct {
    int id;      // object number (1-based)
    int gen;     // generation number (always 0 for new PDFs)
} TspdfRef;

// Tracks where each object was written in the output
typedef struct {
    size_t byte_offsets[TSPDF_MAX_OBJECTS];  // byte offset of each object
    int count;                             // number of objects allocated
} TspdfXref;

// Low-level PDF file writer
typedef struct {
    TspdfBuffer output;       // the raw PDF bytes being built
    TspdfXref xref;        // cross-reference tracking
} TspdfRawWriter;

// Lifecycle
TspdfRawWriter tspdf_raw_writer_create(void);
void tspdf_raw_writer_destroy(TspdfRawWriter *w);

// Allocate the next object number
TspdfRef tspdf_raw_writer_alloc_object(TspdfRawWriter *w);

// Begin/end an indirect object definition: "N 0 obj ... endobj"
void tspdf_raw_writer_begin_object(TspdfRawWriter *w, TspdfRef ref);
void tspdf_raw_writer_end_object(TspdfRawWriter *w);

// Write PDF primitives into the current context
void tspdf_raw_write_null(TspdfRawWriter *w);
void tspdf_raw_write_bool(TspdfRawWriter *w, bool val);
void tspdf_raw_write_int(TspdfRawWriter *w, int val);
void tspdf_raw_write_real(TspdfRawWriter *w, double val);
void tspdf_raw_write_name(TspdfRawWriter *w, const char *name);
void tspdf_raw_write_string(TspdfRawWriter *w, const char *str);
void tspdf_raw_write_string_hex(TspdfRawWriter *w, const uint8_t *data, size_t len);
void tspdf_raw_write_ref(TspdfRawWriter *w, TspdfRef ref);

// Arrays: [ ... ]
void tspdf_raw_write_array_begin(TspdfRawWriter *w);
void tspdf_raw_write_array_end(TspdfRawWriter *w);

// Dictionaries: << ... >>
void tspdf_raw_write_dict_begin(TspdfRawWriter *w);
void tspdf_raw_write_dict_end(TspdfRawWriter *w);
// Convenience: write a /Key followed by a value
void tspdf_raw_write_dict_name_int(TspdfRawWriter *w, const char *key, int val);
void tspdf_raw_write_dict_name_real(TspdfRawWriter *w, const char *key, double val);
void tspdf_raw_write_dict_name_name(TspdfRawWriter *w, const char *key, const char *val);
void tspdf_raw_write_dict_name_ref(TspdfRawWriter *w, const char *key, TspdfRef ref);
void tspdf_raw_write_dict_name_bool(TspdfRawWriter *w, const char *key, bool val);
void tspdf_raw_write_dict_name_string(TspdfRawWriter *w, const char *key, const char *val);

// Stream objects: dict + stream data
// Call begin_object first, then write stream dict entries, then:
void tspdf_raw_write_stream(TspdfRawWriter *w, const uint8_t *data, size_t len);

// Like tspdf_raw_write_stream but compresses with FlateDecode when beneficial
void tspdf_raw_write_stream_compressed(TspdfRawWriter *w, const uint8_t *data, size_t len);

// Write the xref table, trailer, and %%EOF
// info_ref.id == 0 means no Info dictionary
void tspdf_raw_writer_finish(TspdfRawWriter *w, TspdfRef catalog_ref, TspdfRef info_ref);

// Write the final output to a file
bool tspdf_raw_writer_write_to_file(TspdfRawWriter *w, const char *path);

// Raw append (for content streams etc.)
void tspdf_raw_write_raw(TspdfRawWriter *w, const char *str);
void tspdf_raw_write_raw_data(TspdfRawWriter *w, const uint8_t *data, size_t len);

#endif
