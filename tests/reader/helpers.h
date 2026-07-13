/*
 * tests/reader/helpers.h — shared static-inline helpers for the reader test suite.
 *
 * Include AFTER the project headers (tspr.h, deflate.h, etc.) that these
 * helpers depend on.  Every function is static inline so there is no ODR
 * issue across translation units.
 */
#ifndef READER_TEST_HELPERS_H
#define READER_TEST_HELPERS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── basic buffer helpers ─────────────────────────────────────────────────── */

static inline bool appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= cap - *pos) {
        return false;
    }

    *pos += (size_t)written;
    return true;
}

static inline bool bytes_contains(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return false;
    }

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static inline bool bytes_contains_bin(const uint8_t *haystack, size_t haystack_len,
                                      const uint8_t *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || needle_len > haystack_len)
        return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static inline size_t bytes_count(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) {
        return 0;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            count++;
            i += needle_len - 1;
        }
    }
    return count;
}

/* ── file I/O helper ─────────────────────────────────────────────────────── */

static inline uint8_t *wasm_read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return NULL; }
    *out_len = nread;
    return buf;
}

/* ── reader/xref helpers ─────────────────────────────────────────────────── */

/* Resolve a reference object; returns obj unchanged if not a REF. */
static inline TspdfObj *test_resolve_ref(TspdfReader *doc, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num, doc->obj_cache, doc->crypt);
}

static inline TspdfObj *dt_catalog_get(TspdfReader *doc, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(doc->catalog, key));
}

static inline TspdfObj *dt_get(TspdfReader *doc, TspdfObj *dict, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(dict, key));
}

static inline bool dt_str_eq(TspdfObj *s, const char *want) {
    return s && (s->type == TSPDF_OBJ_STRING || s->type == TSPDF_OBJ_NAME) &&
           s->string.len == strlen(want) &&
           memcmp(s->string.data, want, s->string.len) == 0;
}

/* ── cross-reference stream helpers ─────────────────────────────────────── */

static inline void xref_entry4(uint8_t *dst, uint8_t type, size_t offset, uint8_t gen) {
    dst[0] = type;
    dst[1] = (uint8_t)((offset >> 8) & 0xFF);
    dst[2] = (uint8_t)(offset & 0xFF);
    dst[3] = gen;
}

static inline void xref_objstm_entry4(uint8_t *dst, uint16_t stream_obj, uint8_t index) {
    dst[0] = 2;
    dst[1] = (uint8_t)((stream_obj >> 8) & 0xFF);
    dst[2] = (uint8_t)(stream_obj & 0xFF);
    dst[3] = index;
}

/* ── stream encoding helpers ────────────────────────────────────────────── */

static inline uint8_t *ascii_hex_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > (SIZE_MAX - 1) / 2) return NULL;

    uint8_t *out = (uint8_t *)malloc(len * 2 + 1);
    if (!out) return NULL;

    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        out[pos++] = (uint8_t)hex[data[i] >> 4];
        out[pos++] = (uint8_t)hex[data[i] & 0x0F];
    }
    out[pos++] = '>';

    *out_len = pos;
    return out;
}

static inline uint8_t *ascii85_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > ((SIZE_MAX - 2) / 5) * 4) return NULL;

    size_t cap = ((len + 3) / 4) * 5 + 2;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;

    size_t pos = 0;
    size_t i = 0;
    while (i + 4 <= len) {
        uint32_t value = ((uint32_t)data[i] << 24) |
                         ((uint32_t)data[i + 1] << 16) |
                         ((uint32_t)data[i + 2] << 8) |
                         (uint32_t)data[i + 3];
        i += 4;

        char digits[5];
        for (int d = 4; d >= 0; d--) {
            digits[d] = (char)((value % 85) + '!');
            value /= 85;
        }
        for (int d = 0; d < 5; d++) {
            out[pos++] = (uint8_t)digits[d];
        }
    }

    size_t remaining = len - i;
    if (remaining > 0) {
        uint32_t value = 0;
        for (size_t j = 0; j < remaining; j++) {
            value |= (uint32_t)data[i + j] << (24 - j * 8);
        }

        char digits[5];
        for (int d = 4; d >= 0; d--) {
            digits[d] = (char)((value % 85) + '!');
            value /= 85;
        }
        for (size_t d = 0; d < remaining + 1; d++) {
            out[pos++] = (uint8_t)digits[d];
        }
    }

    out[pos++] = '~';
    out[pos++] = '>';

    *out_len = pos;
    return out;
}

static inline uint8_t *run_length_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > SIZE_MAX - (len / 128) - 2) return NULL;

    size_t cap = len + (len / 128) + 2;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;

    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 128) chunk = 128;
        out[pos++] = (uint8_t)(chunk - 1);
        memcpy(out + pos, data + i, chunk);
        pos += chunk;
        i += chunk;
    }
    out[pos++] = 128;

    *out_len = pos;
    return out;
}

/* ── PDF builder helpers ────────────────────────────────────────────────── */

static inline char *make_missing_page_tree_type_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Kids [3 0 R] /Count 1 /MediaBox [0 0 320 420] >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static inline char *make_catalog_features_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n"
                 "<< /Type /Catalog /Pages 2 0 R /PageMode /UseOutlines "
                 "/ViewerPreferences << /DisplayDocTitle true >> "
                 "/Outlines 4 0 R /Names 5 0 R /AcroForm 6 0 R "
                 "/Metadata 7 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Outlines /Count 0 >>\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Dests << /Names [] >> >>\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Fields [] /NeedAppearances true >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Type /Metadata /Subtype /XML /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 8\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 8 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static inline char *dt_make_cyclic_outline_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Title (LOOPA) /Parent 4 0 R /Next 6 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (LOOPB) /Parent 4 0 R /Prev 5 0 R /Next 5 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static inline char *make_object_stream_pdf_with_options(size_t *out_len,
                                                        const char *first_override,
                                                        const char *n_override,
                                                        const char *third_offset_override,
                                                        const char *first_objnum_override,
                                                        bool ascii_hex_filter,
                                                        bool ascii85_filter,
                                                        bool run_length_filter) {
    char obj_stream[1024];
    size_t obj_pos = 0;
    uint8_t *comp = NULL;
    uint8_t *encoded = NULL;

    const char *obj1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>";
    char third_offset_buf[32];
    if (!third_offset_override) {
        snprintf(third_offset_buf, sizeof(third_offset_buf), "%zu", strlen(obj1) + strlen(obj2));
    }
    const char *third_offset = third_offset_override ? third_offset_override : third_offset_buf;
    const char *first_obj_num = first_objnum_override ? first_objnum_override : "1";

    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s 0 2 %zu 3 %s ",
                 first_obj_num, strlen(obj1), third_offset)) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s%s", obj1, obj2, obj3)) {
        return NULL;
    }

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    char first_buf[32];
    if (!first_override) {
        snprintf(first_buf, sizeof(first_buf), "%zu", first);
    }
    const char *first_value = first_override ? first_override : first_buf;

    const char *n_value = n_override ? n_override : "3";
    uint8_t *stream_payload = (uint8_t *)obj_stream;
    size_t stream_payload_len = obj_pos;
    if (ascii_hex_filter || ascii85_filter || run_length_filter) {
        size_t comp_len = 0;
        comp = deflate_compress((const uint8_t *)obj_stream, obj_pos, &comp_len);
        if (!comp) goto fail;
        encoded = run_length_filter ? run_length_encode(comp, comp_len, &stream_payload_len) :
                  ascii85_filter ? ascii85_encode(comp, comp_len, &stream_payload_len) :
                  ascii_hex_encode(comp, comp_len, &stream_payload_len);
        if (!encoded) goto fail;
        stream_payload = encoded;
    }

    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N %s /First %s /Length %zu%s >>\n"
                 "stream\n",
                 n_value, first_value, stream_payload_len,
                 ascii_hex_filter ? " /Filter [/ASCIIHexDecode /FlateDecode]" :
                 ascii85_filter ? " /Filter [/ASCII85Decode /FlateDecode]" :
                 run_length_filter ? " /Filter [/RunLengthDecode /FlateDecode]" : "")) goto fail;
    if (pos + stream_payload_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, stream_payload, stream_payload_len);
    pos += stream_payload_len;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;
    free(encoded);
    free(comp);
    encoded = NULL;
    comp = NULL;

    size_t xref_obj = pos;
    uint8_t entries[6][4];
    xref_entry4(entries[0], 0, 0, 255);
    xref_objstm_entry4(entries[1], 4, 0);
    xref_objstm_entry4(entries[2], 4, 1);
    xref_objstm_entry4(entries[3], 4, 2);
    xref_entry4(entries[4], 1, obj4, 0);
    xref_entry4(entries[5], 1, xref_obj, 0);

    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 6] /Size 6 /Root 1 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > 4096) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(encoded);
    free(comp);
    free(pdf);
    return NULL;
}

static inline char *make_object_stream_pdf(size_t *out_len) {
    return make_object_stream_pdf_with_options(out_len, NULL, NULL, NULL, NULL, false, false, false);
}

/* ── writer-based PDF builder helper ────────────────────────────────────── */

static inline uint8_t *dt_writer_pdf(int npages, bool outlines, bool fields,
                                     const char *tag, const char *font_name,
                                     size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, font_name);
    for (int i = 0; i < npages; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 14);
        tspdf_stream_move_to(page, 72, 700);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s page %d", tag, i + 1);
        tspdf_stream_show_text(page, buf);
        tspdf_stream_end_text(page);
    }
    if (outlines) {
        char t[128];
        snprintf(t, sizeof(t), "%s-CH1", tag);
        int ch1 = tspdf_writer_add_bookmark(w, t, 0);
        snprintf(t, sizeof(t), "%s-CH1-SUB", tag);
        tspdf_writer_add_child_bookmark(w, ch1, t, npages > 1 ? 1 : 0);
        snprintf(t, sizeof(t), "%s-CH2", tag);
        tspdf_writer_add_bookmark(w, t, npages - 1);
    }
    if (fields) {
        char n[128];
        snprintf(n, sizeof(n), "%s_text", tag);
        tspdf_writer_add_text_field(w, 0, n, 72, 600, 200, 20, "hello",
                                    font_name, 12);
        snprintf(n, sizeof(n), "%s_check", tag);
        tspdf_writer_add_checkbox(w, npages - 1, n, 72, 560, 14, true);
    }
    uint8_t *data = NULL;
    TspdfError err = tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    if (err != TSPDF_OK) {
        free(data);
        return NULL;
    }
    return data;
}

#endif /* READER_TEST_HELPERS_H */
