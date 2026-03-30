#include "pdf_writer.h"
#include "../compress/deflate.h"
#include <string.h>
#include <stdlib.h>

TspdfRawWriter tspdf_raw_writer_create(void) {
    TspdfRawWriter w = {0};
    w.output = tspdf_buffer_create(64 * 1024);  // 64KB initial
    w.xref.count = 0;

    // PDF header
    tspdf_buffer_append_str(&w.output, "%PDF-1.7\n");
    // Binary comment to signal binary content (recommended by spec)
    uint8_t bin_marker[] = {'%', 0xE2, 0xE3, 0xCF, 0xD3, '\n'};
    tspdf_buffer_append(&w.output, bin_marker, sizeof(bin_marker));

    return w;
}

void tspdf_raw_writer_destroy(TspdfRawWriter *w) {
    tspdf_buffer_destroy(&w->output);
}

TspdfRef tspdf_raw_writer_alloc_object(TspdfRawWriter *w) {
    TspdfRef ref;
    if (w->xref.count >= TSPDF_MAX_OBJECTS - 1) {
        ref.id = 0;
        ref.gen = 0;
        return ref;
    }
    ref.id = ++w->xref.count;  // 1-based
    ref.gen = 0;
    return ref;
}

void tspdf_raw_writer_begin_object(TspdfRawWriter *w, TspdfRef ref) {
    // Record byte offset for xref
    if (ref.id <= 0 || ref.id >= TSPDF_MAX_OBJECTS) {
        return;
    }
    w->xref.byte_offsets[ref.id] = w->output.len;
    tspdf_buffer_printf(&w->output, "%d %d obj\n", ref.id, ref.gen);
}

void tspdf_raw_writer_end_object(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, "endobj\n");
}

void tspdf_raw_write_null(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, "null ");
}

void tspdf_raw_write_bool(TspdfRawWriter *w, bool val) {
    tspdf_buffer_append_str(&w->output, val ? "true " : "false ");
}

void tspdf_raw_write_int(TspdfRawWriter *w, int val) {
    tspdf_buffer_printf(&w->output, "%d ", val);
}

void tspdf_raw_write_real(TspdfRawWriter *w, double val) {
    tspdf_buffer_printf(&w->output, "%.4f ", val);
}

void tspdf_raw_write_name(TspdfRawWriter *w, const char *name) {
    tspdf_buffer_printf(&w->output, "/%s ", name);
}

void tspdf_raw_write_string(TspdfRawWriter *w, const char *str) {
    tspdf_buffer_append_byte(&w->output, '(');
    // Escape special characters
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '(':  tspdf_buffer_append_str(&w->output, "\\("); break;
            case ')':  tspdf_buffer_append_str(&w->output, "\\)"); break;
            case '\\': tspdf_buffer_append_str(&w->output, "\\\\"); break;
            default:   tspdf_buffer_append_byte(&w->output, (uint8_t)*p); break;
        }
    }
    tspdf_buffer_append_str(&w->output, ") ");
}

void tspdf_raw_write_string_hex(TspdfRawWriter *w, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(&w->output, '<');
    for (size_t i = 0; i < len; i++) {
        tspdf_buffer_printf(&w->output, "%02X", data[i]);
    }
    tspdf_buffer_append_str(&w->output, "> ");
}

void tspdf_raw_write_ref(TspdfRawWriter *w, TspdfRef ref) {
    tspdf_buffer_printf(&w->output, "%d %d R ", ref.id, ref.gen);
}

void tspdf_raw_write_array_begin(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, "[ ");
}

void tspdf_raw_write_array_end(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, "] ");
}

void tspdf_raw_write_dict_begin(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, "<< ");
}

void tspdf_raw_write_dict_end(TspdfRawWriter *w) {
    tspdf_buffer_append_str(&w->output, ">> ");
}

void tspdf_raw_write_dict_name_int(TspdfRawWriter *w, const char *key, int val) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_int(w, val);
}

void tspdf_raw_write_dict_name_real(TspdfRawWriter *w, const char *key, double val) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_real(w, val);
}

void tspdf_raw_write_dict_name_name(TspdfRawWriter *w, const char *key, const char *val) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_name(w, val);
}

void tspdf_raw_write_dict_name_ref(TspdfRawWriter *w, const char *key, TspdfRef ref) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_ref(w, ref);
}

void tspdf_raw_write_dict_name_bool(TspdfRawWriter *w, const char *key, bool val) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_bool(w, val);
}

void tspdf_raw_write_dict_name_string(TspdfRawWriter *w, const char *key, const char *val) {
    tspdf_raw_write_name(w, key);
    tspdf_raw_write_string(w, val);
}

void tspdf_raw_write_stream(TspdfRawWriter *w, const uint8_t *data, size_t len) {
    tspdf_buffer_printf(&w->output, "/Length %zu ", len);
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\nstream\n");
    tspdf_buffer_append(&w->output, data, len);
    tspdf_buffer_append_str(&w->output, "\nendstream\n");
}

void tspdf_raw_write_stream_compressed(TspdfRawWriter *w, const uint8_t *data, size_t len) {
    // Skip compression for small streams — the HashChain allocation (~160KB)
    // and LZ77 overhead exceeds any benefit for tiny data
    if (len < 256) {
        tspdf_raw_write_stream(w, data, len);
        return;
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);

    if (comp && comp_len < len) {
        // Compressed is smaller — use it
        tspdf_raw_write_name(w, "Filter");
        tspdf_raw_write_name(w, "FlateDecode");
        tspdf_buffer_printf(&w->output, "/Length %zu ", comp_len);
        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\nstream\n");
        tspdf_buffer_append(&w->output, comp, comp_len);
        tspdf_buffer_append_str(&w->output, "\nendstream\n");
        free(comp);
    } else {
        // Compression didn't help or failed — store uncompressed
        if (comp) free(comp);
        tspdf_buffer_printf(&w->output, "/Length %zu ", len);
        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\nstream\n");
        tspdf_buffer_append(&w->output, data, len);
        tspdf_buffer_append_str(&w->output, "\nendstream\n");
    }
}

void tspdf_raw_writer_finish(TspdfRawWriter *w, TspdfRef catalog_ref, TspdfRef info_ref) {
    // Write xref table
    size_t xref_offset = w->output.len;
    tspdf_buffer_append_str(&w->output, "xref\n");
    tspdf_buffer_printf(&w->output, "0 %d\n", w->xref.count + 1);

    // Object 0: free object (head of free list)
    tspdf_buffer_append_str(&w->output, "0000000000 65535 f \n");

    // Objects 1..count
    for (int i = 1; i <= w->xref.count; i++) {
        tspdf_buffer_printf(&w->output, "%010lu 00000 n \n", (unsigned long)w->xref.byte_offsets[i]);
    }

    // Trailer
    tspdf_buffer_append_str(&w->output, "trailer\n");
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_int(w, "Size", w->xref.count + 1);
    tspdf_raw_write_dict_name_ref(w, "Root", catalog_ref);
    if (info_ref.id > 0) {
        tspdf_raw_write_dict_name_ref(w, "Info", info_ref);
    }
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");

    // startxref
    tspdf_buffer_printf(&w->output, "startxref\n%zu\n", xref_offset);
    tspdf_buffer_append_str(&w->output, "%%EOF\n");
}

bool tspdf_raw_writer_write_to_file(TspdfRawWriter *w, const char *path) {
    if (w->output.error) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(w->output.data, 1, w->output.len, f);
    fclose(f);
    return written == w->output.len;
}

void tspdf_raw_write_raw(TspdfRawWriter *w, const char *str) {
    tspdf_buffer_append_str(&w->output, str);
}

void tspdf_raw_write_raw_data(TspdfRawWriter *w, const uint8_t *data, size_t len) {
    tspdf_buffer_append(&w->output, data, len);
}
