#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "../test_framework.h"
#include "../../src/reader/tspr.h"
#include "../../src/reader/tspr_overlay.h"
#include "../../src/reader/tspr_internal.h"
#include "../../src/pdf/pdf_stream.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/util/arena.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/font/ttf_parser.h"
#include "../../src/font/font_fallback.h"
#include "../../src/crypto/md5.h"
#include "../../include/tspdf/version.h"

#include "../../src/reader/tspr_text.h"

static bool bytes_contains(const uint8_t *haystack, size_t haystack_len, const char *needle) {
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

static TspdfObj *test_resolve_ref(TspdfReader *doc, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num, doc->obj_cache, doc->crypt);
}

static bool appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
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

static void xref_entry4(uint8_t *dst, uint8_t type, size_t offset, uint8_t gen) {
    dst[0] = type;
    dst[1] = (uint8_t)((offset >> 8) & 0xFF);
    dst[2] = (uint8_t)(offset & 0xFF);
    dst[3] = gen;
}

static void xref_objstm_entry4(uint8_t *dst, uint16_t stream_obj, uint8_t index) {
    dst[0] = 2;
    dst[1] = (uint8_t)((stream_obj >> 8) & 0xFF);
    dst[2] = (uint8_t)(stream_obj & 0xFF);
    dst[3] = index;
}

static uint8_t *ascii_hex_encode(const uint8_t *data, size_t len, size_t *out_len) {
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

static uint8_t *ascii85_encode(const uint8_t *data, size_t len, size_t *out_len) {
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

static uint8_t *run_length_encode(const uint8_t *data, size_t len, size_t *out_len) {
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

static char *make_object_stream_pdf_with_options(size_t *out_len,
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

static char *make_object_stream_pdf(size_t *out_len) {
    return make_object_stream_pdf_with_options(out_len, NULL, NULL, NULL, NULL, false, false, false);
}

static char *make_missing_page_tree_type_pdf(size_t *out_len) {
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

static char *make_inherited_page_attrs_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *content = "BT /F1 12 Tf 72 720 Td (Hi) Tj ET";
    size_t content_len = strlen(content);

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/MediaBox 4 0 R /Rotate 5 0 R "
                 "/BleedBox [5 5 295 395] "
                 "/TrimBox [10 10 290 390] "
                 "/ArtBox [15 15 285 385] "
                 "/Resources << /Font << /F1 6 0 R >> >> >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 7 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n[0 0 300 400]\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n90\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 content_len, content)) goto fail;

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

static char *make_cyclic_page_tree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [2 0 R] /Count 1 /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 3\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 3 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static bool read_page_box(TspdfReader *doc, size_t idx, const char *key, double out[4]) {
    TspdfReaderPage *page = tspdf_reader_get_page(doc, idx);
    if (!page) return false;
    TspdfObj *box = tspdf_dict_get(page->page_dict, key);
    if (!box || box->type != TSPDF_OBJ_ARRAY || box->array.count < 4) return false;
    for (int i = 0; i < 4; i++) {
        TspdfObj *it = &box->array.items[i];
        if (it->type == TSPDF_OBJ_INT) out[i] = (double)it->integer;
        else if (it->type == TSPDF_OBJ_REAL) out[i] = it->real;
        else return false;
    }
    return true;
}

static char *make_v4_identity_stmf_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    static const char *O_HEX =
        "77b8fb098022d3ab34237ea5643c08710ea5123fc5f88bf993a68cca5f12b40f";
    static const char *U_HEX =
        "e8dea6f86ac2bec59be9e09da2d4f1d20021446990b9e4114071a4d9104984c1";
    static const char *ID_HEX = "c894ca13f80c954088d2e3b894bf9c45";
    static const char *CONTENT = "BT /F1 12 Tf (IDENTITYSTREAM) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(CONTENT), CONTENT)) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Filter /Standard /V 4 /R 4 /Length 128 "
                 "/P -4 /O <%s> /U <%s> "
                 "/CF << /StdCF << /CFM /AESV2 /Length 16 /AuthEvent /DocOpen >> >> "
                 "/StmF /Identity /StrF /StdCF >>\nendobj\n",
                 O_HEX, U_HEX)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 6\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R /Encrypt 5 0 R "
                 "/ID [<%s><%s>] >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, ID_HEX, ID_HEX, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_pinned_trees_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n"
                 "<< /Type /Catalog /Pages 2 0 R /Lang (en) "
                 "/StructTreeRoot 6 0 R /Outlines 7 0 R /Dests 8 0 R "
                 "/Names 9 0 R /PageLabels 10 0 R /AcroForm 11 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 5 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Length 20 >>\nstream\nq 1 0 0 1 0 0 cm Q\n\nendstream\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Type /StructTreeRoot /K 12 0 R >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Type /Outlines /Count 1 /First 12 0 R >>\nendobj\n")) goto fail;

    size_t obj8 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /SomeDest 12 0 R >>\nendobj\n")) goto fail;

    size_t obj9 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "9 0 obj\n<< /Dests << /Names [(d1) 12 0 R] >> >>\nendobj\n")) goto fail;

    size_t obj10 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "10 0 obj\n<< /Nums [0 12 0 R] >>\nendobj\n")) goto fail;

    size_t obj11 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "11 0 obj\n<< /Fields [12 0 R] >>\nendobj\n")) goto fail;

    size_t obj12 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "12 0 obj\n<< /Marker (PINNEDPAYLOAD) >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos,
                 "xref\n"
                 "0 13\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 13 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9,
                 obj10, obj11, obj12, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static bool append_raw(char *buf, size_t cap, size_t *pos, const void *data, size_t len) {
    if (len > cap - *pos) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static uint32_t test_adler32(const uint8_t *data, size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

static char *make_compressible_payload(size_t *payload_len) {
    const char *line = "0.2 0.4 0.6 rg 72 700 468 -650 re f\n";
    size_t line_len = strlen(line);
    size_t reps = 300;  // ~10.8KB, well above the old 4KB recompress floor
    char *payload = (char *)malloc(line_len * reps + 1);
    if (!payload) return NULL;
    for (size_t i = 0; i < reps; i++) {
        memcpy(payload + i * line_len, line, line_len);
    }
    payload[line_len * reps] = '\0';
    *payload_len = line_len * reps;
    return payload;
}

static char *make_unfiltered_stream_pdf(const char *payload, size_t payload_len,
                                        size_t *out_len) {
    size_t cap = payload_len + 2048;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n<< /Length %zu >>\nstream\n", payload_len)) goto fail;
    if (!append_raw(pdf, cap, &pos, payload, payload_len)) goto fail;
    if (!appendf(pdf, cap, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_stored_flate_stream_pdf(const char *payload, size_t payload_len,
                                          size_t *out_len) {
    // zlib header + stored blocks (5 bytes overhead per 65535) + adler32
    size_t flate_cap = 2 + payload_len + 5 * (payload_len / 65535 + 1) + 4;
    uint8_t *flate = (uint8_t *)malloc(flate_cap);
    if (!flate) return NULL;

    size_t fpos = 0;
    flate[fpos++] = 0x78;
    flate[fpos++] = 0x01;
    size_t remaining = payload_len;
    const uint8_t *src = (const uint8_t *)payload;
    while (remaining > 0) {
        uint16_t block = remaining > 65535 ? 65535 : (uint16_t)remaining;
        flate[fpos++] = (remaining == block) ? 1 : 0;  // BFINAL, BTYPE=00
        flate[fpos++] = (uint8_t)(block & 0xFF);
        flate[fpos++] = (uint8_t)(block >> 8);
        flate[fpos++] = (uint8_t)(~block & 0xFF);
        flate[fpos++] = (uint8_t)((~block >> 8) & 0xFF);
        memcpy(flate + fpos, src, block);
        fpos += block;
        src += block;
        remaining -= block;
    }
    uint32_t adler = test_adler32((const uint8_t *)payload, payload_len);
    flate[fpos++] = (uint8_t)(adler >> 24);
    flate[fpos++] = (uint8_t)(adler >> 16);
    flate[fpos++] = (uint8_t)(adler >> 8);
    flate[fpos++] = (uint8_t)adler;

    size_t cap = fpos + 2048;
    char *pdf = (char *)malloc(cap);
    if (!pdf) { free(flate); return NULL; }

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Length %zu /Filter /FlateDecode >>\nstream\n", fpos)) goto fail;
    if (!append_raw(pdf, cap, &pos, flate, fpos)) goto fail;
    if (!appendf(pdf, cap, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    free(flate);
    *out_len = pos;
    return pdf;

fail:
    free(flate);
    free(pdf);
    return NULL;
}

static TspdfObj *first_page_contents(TspdfReader *doc) {
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    if (!page) return NULL;
    return test_resolve_ref(doc, tspdf_dict_get(page->page_dict, "Contents"));
}

static char *make_many_small_objects_pdf(size_t *out_len, int count) {
    size_t cap = 65536 + (size_t)count * 128;
    char *pdf = (char *)malloc(cap);
    size_t *off = (size_t *)calloc((size_t)count + 8, sizeof(size_t));
    if (!pdf || !off) {
        free(pdf);
        free(off);
        return NULL;
    }

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> /ExtGState << ")) goto fail;
    for (int i = 0; i < count; i++) {
        if (!appendf(pdf, cap, &pos, "/GS%d %d 0 R ", i, 6 + i)) goto fail;
    }
    if (!appendf(pdf, cap, &pos, ">> >> >>\nendobj\n")) goto fail;

    const char *content = "BT /F1 12 Tf 72 720 Td (Hello ObjStm) Tj ET";
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;

    off[5] = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;

    for (int i = 0; i < count; i++) {
        off[6 + i] = pos;
        if (!appendf(pdf, cap, &pos,
                     "%d 0 obj\n<< /Type /ExtGState /CA 0.%d /LW %d >>\nendobj\n",
                     6 + i, i % 9 + 1, i % 7 + 1)) goto fail;
    }

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos, "xref\n0 %d\n0000000000 65535 f \n", count + 6)) goto fail;
    for (int i = 1; i < count + 6; i++) {
        if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, cap, &pos,
                 "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 count + 6, xref)) goto fail;

    free(off);
    *out_len = pos;
    return pdf;

fail:
    free(off);
    free(pdf);
    return NULL;
}

static char *make_titled_info_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Title (KeepMeTitle99) /Author (Ann Author) >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static bool find_id_halves(const uint8_t *buf, size_t len,
                           char *id0, char *id1, size_t cap) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (memcmp(buf + i, "/ID", 3) != 0) continue;
        size_t p = i + 3;
        char *dst[2] = {id0, id1};
        for (int half = 0; half < 2; half++) {
            while (p < len && buf[p] != '<') p++;
            if (p >= len) return false;
            p++;
            size_t n = 0;
            while (p < len && buf[p] != '>') {
                if (n + 1 >= cap) return false;
                dst[half][n++] = (char)buf[p++];
            }
            if (p >= len) return false;
            dst[half][n] = '\0';
            p++;
        }
        return true;
    }
    return false;
}

static TspdfObj *dt_catalog_get(TspdfReader *doc, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(doc->catalog, key));
}

static TspdfObj *dt_get(TspdfReader *doc, TspdfObj *dict, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(dict, key));
}

static bool dt_str_eq(TspdfObj *s, const char *want) {
    return s && (s->type == TSPDF_OBJ_STRING || s->type == TSPDF_OBJ_NAME) &&
           s->string.len == strlen(want) &&
           memcmp(s->string.data, want, s->string.len) == 0;
}

static size_t dt_dest_page(TspdfReader *doc, TspdfObj *item) {
    TspdfObj *dest = dt_get(doc, item, "Dest");
    if (dest && dest->type == TSPDF_OBJ_DICT) {
        dest = dt_get(doc, dest, "D");
    }
    if (!dest || dest->type != TSPDF_OBJ_ARRAY || dest->array.count == 0) {
        return SIZE_MAX;
    }
    TspdfObj *p = &dest->array.items[0];
    if (p->type != TSPDF_OBJ_REF) return SIZE_MAX;
    for (size_t i = 0; i < doc->pages.count; i++) {
        if (doc->pages.pages[i].obj_num == p->ref.num) return i;
    }
    return SIZE_MAX;
}

static uint8_t *dt_writer_pdf(int npages, bool outlines, bool fields,
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

static char *dt_make_named_dest_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[16] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R "
                 "/Names << /Dests 8 0 R >> /Dests 11 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R 5 0 R] /Count 2 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Outlines /First 6 0 R /Last 14 0 R /Count 4 >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (NAMED1) /Parent 3 0 R /Next 7 0 R "
                 "/Dest (target1) >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Title (NAMED2) /Parent 3 0 R /Prev 6 0 R /Next 13 0 R "
                 "/A << /S /GoTo /D (target2) >> >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /Names [(target1) 9 0 R (target2) 10 0 R] >>\nendobj\n")) goto fail;
    off[9] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "9 0 obj\n[4 0 R /XYZ 0 792 0]\nendobj\n")) goto fail;
    off[10] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "10 0 obj\n<< /D [5 0 R /Fit] >>\nendobj\n")) goto fail;
    off[11] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "11 0 obj\n<< /target3 12 0 R >>\nendobj\n")) goto fail;
    off[12] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "12 0 obj\n[4 0 R /Fit]\nendobj\n")) goto fail;
    off[13] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "13 0 obj\n<< /Title (NAMED3) /Parent 3 0 R /Prev 7 0 R /Next 14 0 R "
                 "/Dest /target3 >>\nendobj\n")) goto fail;
    off[14] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "14 0 obj\n<< /Title (URIITEM) /Parent 3 0 R /Prev 13 0 R "
                 "/A << /S /URI /URI (http://example.org/x) >> >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 15\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 14; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 15 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *dt_make_field_kids_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[16] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [6 0 R] /DA (/Helv 0 Tf 0 g) "
                 "/NeedAppearances true >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [7 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [8 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /T (parentfield) /FT /Tx /Kids [7 0 R 8 0 R] >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Type /Annot /Subtype /Widget /Rect [10 10 60 30] "
                 "/Parent 6 0 R /P 3 0 R /F 4 >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /Type /Annot /Subtype /Widget /Rect [10 40 60 60] "
                 "/Parent 6 0 R /F 4 >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 8; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *dt_make_cyclic_nametree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R "
                 "/Names << /Dests 5 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Outlines /First 6 0 R /Last 6 0 R /Count 1 >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos, "5 0 obj\n<< /Kids [")) goto fail;
    for (int i = 0; i < 50; i++) {
        if (!appendf(pdf, 8192, &pos, "5 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (CYCITEM) /Parent 4 0 R "
                 "/Dest (loopname) >>\nendobj\n")) goto fail;

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

static char *dt_make_cyclic_field_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [4 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos, "4 0 obj\n<< /T (loopfield) /FT /Tx /Kids [")) goto fail;
    for (int i = 0; i < 40; i++) {
        if (!appendf(pdf, 8192, &pos, "4 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *dt_make_cyclic_outline_pdf(size_t *out_len) {
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

static const uint8_t att_payload[] = {
    'H', 'e', 'l', 'l', 'o', 0x00, 0xFF, 0x01, '\n', 'w', 'o', 'r', 'l', 'd',
    0x80, 0x00, 'e', 'n', 'd'
};

static TspdfReader *att_add_and_reopen(const char *name, const uint8_t *data,
                                       size_t len, const char *desc,
                                       uint8_t **out_buf) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(2, false, false, "AT", "Helvetica", &src_len);
    if (!src) return NULL;

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    if (!doc) {
        free(src);
        return NULL;
    }
    if (tspdf_reader_attachment_add(doc, name, data, len, desc) != TSPDF_OK) {
        tspdf_reader_destroy(doc);
        free(src);
        return NULL;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    tspdf_reader_destroy(doc);
    free(src);
    if (err != TSPDF_OK) {
        free(out);
        return NULL;
    }

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    if (!re) {
        free(out);
        return NULL;
    }
    *out_buf = out;
    return re;
}

static char *att_make_cyclic_embeddedfiles_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Names [(cyc.txt) 5 0 R] /Kids [")) goto fail;
    for (int i = 0; i < 40; i++) {
        if (!appendf(pdf, 8192, &pos, "4 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Filespec /F (cyc.txt) /UF (cyc.txt) "
                 "/EF << /F 6 0 R >> >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Type /EmbeddedFile /Length 9 >>\nstream\n"
                 "CYCBYTES!\nendstream\nendobj\n")) goto fail;

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

static char *att_make_params_pdf(size_t *out_len, const char *stream_dict_extra) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Names [(liar.bin) 5 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /Filespec /F (liar.bin) /UF (liar.bin) "
                 "/EF << /F 6 0 R >> >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /EmbeddedFile /Length 9 %s >>\nstream\n"
                 "REALBYTES\nendstream\nendobj\n", stream_dict_extra)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *att_make_foreign_names_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[10] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    // Keys sorted by raw bytes: (caf\xE9.txt) < <FEFF...>.
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Names [(caf\xE9.txt) 5 0 R "
                 "<FEFF00FC006E00EF0063006F00640065002E007400780074> 7 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /Filespec /F (caf\xE9.txt) /UF (caf\xE9.txt) "
                 "/EF << /F 6 0 R >> >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /EmbeddedFile /Length 12 >>\nstream\n"
                 "hello pdfdoc\nendstream\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Type /Filespec /F (unicode.txt) "
                 "/UF <FEFF00FC006E00EF0063006F00640065002E007400780074> "
                 "/EF << /F 8 0 R >> >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "8 0 obj\n<< /Type /EmbeddedFile /Length 11 >>\nstream\n"
                 "hello utf16\nendstream\nendobj\n")) goto fail;

    {
        size_t xref = pos;
        if (!appendf(pdf, 4096, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 8; i++) {
            if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, 4096, &pos,
                     "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *pl_make_labeled_pdf(bool cyclic, size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    size_t offs[10];
    offs[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /PageLabels 8 0 R >>\nendobj\n"))
        goto fail;
    offs[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] "
                 "/Count 5 >>\nendobj\n")) goto fail;
    for (int i = 3; i <= 7; i++) {
        offs[i] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
                     "/MediaBox [0 0 612 792] >>\nendobj\n", i)) goto fail;
    }
    offs[8] = pos;
    if (cyclic) {
        if (!appendf(pdf, 8192, &pos,
                     "8 0 obj\n<< /Kids [9 0 R] >>\nendobj\n")) goto fail;
        offs[9] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "9 0 obj\n<< /Kids [9 0 R 8 0 R] "
                     "/Nums [0 << /S /r >> 3 << /S /D >>] >>\nendobj\n")) goto fail;
    } else {
        if (!appendf(pdf, 8192, &pos,
                     "8 0 obj\n<< /Nums [0 << /S /r >> 3 << /S /D >>] >>\nendobj\n"))
            goto fail;
        offs[9] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "9 0 obj\n<< /Unused true >>\nendobj\n")) goto fail;
    }

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos,
                 "xref\n0 10\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 9; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", offs[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static bool pl_expect_range(TspdfReader *doc, TspdfObj *nums, size_t pair_idx,
                            int64_t key, char style, int64_t st) {
    if (!nums || nums->type != TSPDF_OBJ_ARRAY) return false;
    if (nums->array.count < (pair_idx + 1) * 2) return false;
    TspdfObj *k = &nums->array.items[pair_idx * 2];
    if (k->type != TSPDF_OBJ_INT || k->integer != key) return false;
    TspdfObj *label = test_resolve_ref(doc, &nums->array.items[pair_idx * 2 + 1]);
    if (!label || label->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *s = tspdf_dict_get(label, "S");
    if (style == 0) {
        if (s != NULL) return false;
    } else {
        char want[2] = {style, '\0'};
        if (!dt_str_eq(s, want)) return false;
    }
    TspdfObj *stv = tspdf_dict_get(label, "St");
    if (st == 1) {
        if (stv != NULL) return false;
    } else {
        if (!stv || stv->type != TSPDF_OBJ_INT || stv->integer != st) return false;
    }
    return true;
}

TEST(test_extract_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};  // extract first and third
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 2);

    // Round-trip to verify valid PDF
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 2);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

TEST(test_delete_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};  // delete second page
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(result), 2);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};
    TspdfReader *rotated = tspdf_reader_rotate(doc, pages, 2, 90, &err);
    ASSERT(rotated != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save and reopen to verify rotate persisted
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(rotated, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 0)->rotate, 90);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 1)->rotate, 0);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 2)->rotate, 90);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_pages_normalizes_negative_angle) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *rotated = tspdf_reader_rotate(doc, pages, 1, -90, &err);
    ASSERT(rotated != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(rotated, 0)->rotate, 270);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(rotated, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 0)->rotate, 270);

    TspdfObj *rotate = tspdf_dict_get(tspdf_reader_get_page(reopened, 0)->page_dict, "Rotate");
    ASSERT(rotate != NULL);
    ASSERT_EQ_INT(rotate->type, TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)rotate->integer, 270);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_explicit_box) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};
    double box[4] = {50, 60, 400, 700};
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 2, box, &err);
    ASSERT(cropped != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(cropped, &out, &out_len), TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);

    double cb[4];
    ASSERT(read_page_box(reopened, 0, "CropBox", cb));
    ASSERT(cb[0] == 50 && cb[1] == 60 && cb[2] == 400 && cb[3] == 700);
    // Page 1 (index 1) was not selected: no CropBox.
    double none[4];
    ASSERT(!read_page_box(reopened, 1, "CropBox", none));
    ASSERT(read_page_box(reopened, 2, "CropBox", cb));
    ASSERT(cb[0] == 50 && cb[2] == 400);

    // Content (text) still present — crop only clips the view.
    const char *txt = tspdf_reader_page_text(reopened, 0, &err);
    ASSERT(txt != NULL);
    ASSERT(strstr(txt, "Page 1") != NULL);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(cropped);
    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_clamps_to_mediabox) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    double mb[4];
    ASSERT(read_page_box(doc, 0, "MediaBox", mb));

    // Box larger than the MediaBox on all sides — must clamp to the media.
    size_t pages[] = {0};
    double box[4] = {-100, -100, mb[2] + 500, mb[3] + 500};
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped != NULL);

    double cb[4];
    ASSERT(read_page_box(cropped, 0, "CropBox", cb));
    ASSERT(cb[0] == mb[0] && cb[1] == mb[1] && cb[2] == mb[2] && cb[3] == mb[3]);

    tspdf_reader_destroy(cropped);
    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_degenerate_rejected) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    double box[4] = {200, 100, 100, 300};  // x1 < x0
    err = TSPDF_OK;
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_out_of_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {5};
    double box[4] = {10, 10, 100, 100};
    err = TSPDF_OK;
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
}

TEST(test_scale_factor_half) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    double mb0[4];
    ASSERT(read_page_box(doc, 0, "MediaBox", mb0));
    double w0 = mb0[2] - mb0[0], h0 = mb0[3] - mb0[1];

    size_t pages[] = {0};
    TspdfReader *scaled = tspdf_reader_scale(doc, pages, 1, 0.5, 0.5, &err);
    ASSERT(scaled != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(scaled, &out, &out_len), TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);

    double mb[4];
    ASSERT(read_page_box(reopened, 0, "MediaBox", mb));
    double w = mb[2] - mb[0], h = mb[3] - mb[1];
    ASSERT(w > w0 * 0.5 - 0.01 && w < w0 * 0.5 + 0.01);
    ASSERT(h > h0 * 0.5 - 0.01 && h < h0 * 0.5 + 0.01);

    // Unscaled page 1 keeps its original dimensions.
    double mb1[4];
    ASSERT(read_page_box(reopened, 1, "MediaBox", mb1));
    ASSERT((mb1[2] - mb1[0]) > w0 - 0.01 && (mb1[2] - mb1[0]) < w0 + 0.01);

    // Text still extractable after the content-transform wrap.
    const char *txt = tspdf_reader_page_text(reopened, 0, &err);
    ASSERT(txt != NULL);
    ASSERT(strstr(txt, "Page 1") != NULL);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(scaled);
    tspdf_reader_destroy(doc);
}

TEST(test_scale_nonuniform_and_cropbox) {
    TspdfError err;
    // Page with both MediaBox and CropBox; scale must transform both.
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] "
        "/CropBox [10 20 190 380] /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 8 >>\nstream\nq 1 0 0\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *scaled = tspdf_reader_scale(doc, pages, 1, 2.0, 3.0, &err);
    ASSERT(scaled != NULL);

    double mb[4], cb[4];
    ASSERT(read_page_box(scaled, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0 && mb[2] == 400 && mb[3] == 1200);
    ASSERT(read_page_box(scaled, 0, "CropBox", cb));
    ASSERT(cb[0] == 20 && cb[1] == 60 && cb[2] == 380 && cb[3] == 1140);

    tspdf_reader_destroy(scaled);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_from_letter) {
    TspdfError err;
    // Letter-size page (612 x 792) with text; resize to A4 (595.28 x 841.89).
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/CropBox [10 10 602 782] /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 595.27 && mb[2] < 595.29);
    ASSERT(mb[3] > 841.88 && mb[3] < 841.90);
    // CropBox dropped by resize.
    double cb[4];
    ASSERT(!read_page_box(r, 0, "CropBox", cb));

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_rotate90_viewed_portrait) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] "
        "/Rotate 90 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // /Rotate is preserved.
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 90);

    // Stored MediaBox is the SWAPPED extents (target_h x target_w).
    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 841.88 && mb[2] < 841.90);   // stored width  = target_h
    ASSERT(mb[3] > 595.27 && mb[3] < 595.29);   // stored height = target_w

    // Viewed dims after applying /Rotate 90 (swap): 595.28 x 841.89 => portrait.
    int rot = tspdf_reader_get_page(r, 0)->rotate;
    double stored_w = mb[2] - mb[0];
    double stored_h = mb[3] - mb[1];
    double viewed_w = (rot == 90 || rot == 270) ? stored_h : stored_w;
    double viewed_h = (rot == 90 || rot == 270) ? stored_w : stored_h;
    ASSERT(viewed_w > 595.27 && viewed_w < 595.29);
    ASSERT(viewed_h > 841.88 && viewed_h < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_rotate270_viewed_portrait) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] "
        "/Rotate 270 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 270);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    int rot = tspdf_reader_get_page(r, 0)->rotate;
    double stored_w = mb[2] - mb[0];
    double stored_h = mb[3] - mb[1];
    double viewed_w = (rot == 90 || rot == 270) ? stored_h : stored_w;
    double viewed_h = (rot == 90 || rot == 270) ? stored_w : stored_h;
    ASSERT(viewed_w > 595.27 && viewed_w < 595.29);
    ASSERT(viewed_h > 841.88 && viewed_h < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_rotate90_preserves_viewed_landscape) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Rotate 90 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 90);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    // Stored box stays portrait so the viewed page is A4 landscape.
    ASSERT(mb[2] > 595.27 && mb[2] < 595.29);
    ASSERT(mb[3] > 841.88 && mb[3] < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_landscape_page_stays_landscape) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 841.88 && mb[2] < 841.90);
    ASSERT(mb[3] > 595.27 && mb[3] < 595.29);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

TEST(test_page_crop_box_exposed) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/CropBox [100 100 400 500] >>\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->has_crop_box);
    ASSERT(page->crop_box[0] == 100 && page->crop_box[1] == 100 &&
           page->crop_box[2] == 400 && page->crop_box[3] == 500);
    tspdf_reader_destroy(doc);

    // A page without a CropBox reports none.
    doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(!page->has_crop_box);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_rejects_nonpositive) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0};
    err = TSPDF_OK;
    ASSERT(tspdf_reader_resize_to(doc, pages, 1, 0.0, 800.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    tspdf_reader_destroy(doc);
}

TEST(test_scale_rejects_nonpositive_factor) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    err = TSPDF_OK;
    ASSERT(tspdf_reader_scale(doc, pages, 1, 0.0, 1.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    err = TSPDF_OK;
    ASSERT(tspdf_reader_scale(doc, pages, 1, 1.0, -2.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(doc);
}

TEST(test_reorder_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t order[] = {2, 0, 1};
    TspdfReader *reordered = tspdf_reader_reorder(doc, order, 3, &err);
    ASSERT(reordered != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reordered), 3);

    tspdf_reader_destroy(reordered);
    tspdf_reader_destroy(doc);
}

TEST(test_merge_documents) {
    TspdfError err;
    TspdfReader *doc1 = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc1 != NULL);
    TspdfReader *doc2 = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc2 != NULL);

    TspdfReader *docs[] = {doc1, doc2};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(merged), 4);

    // Round-trip
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 4);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc2);
    tspdf_reader_destroy(doc1);
}

TEST(test_save_rejects_missing_stream_source) {
    TspdfError err;
    TspdfReader *doc1 = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc1 != NULL);

    TspdfReader *docs[] = {doc1};
    TspdfReader *merged = tspdf_reader_merge(docs, 1, &err);
    ASSERT(merged != NULL);
    ASSERT(merged->data == NULL);

    TspdfObj *stream = NULL;
    for (size_t i = 1; i < merged->xref.count; i++) {
        if (merged->obj_cache[i] && merged->obj_cache[i]->type == TSPDF_OBJ_STREAM) {
            stream = merged->obj_cache[i];
            break;
        }
    }
    ASSERT(stream != NULL);
    ASSERT(stream->stream.data != NULL);

    uint8_t *old_data = stream->stream.data;
    stream->stream.data = NULL;
    stream->stream.self_contained = false;
    stream->stream.raw_offset = 1;
    stream->stream.raw_len = 1;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT(err != TSPDF_OK);
    ASSERT(out == NULL);
    ASSERT_EQ_SIZE(out_len, 0);

    err = tspdf_reader_save_to_memory_encrypted(merged, &out, &out_len,
                                                  "user", "owner", 0xFFFFFFFC, 128);
    ASSERT(err != TSPDF_OK);
    ASSERT(out == NULL);
    ASSERT_EQ_SIZE(out_len, 0);

    free(old_data);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc1);
}

TEST(test_merge_rejects_invalid_source_stream_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);

    TspdfObj *contents = tspdf_dict_get(page->page_dict, "Contents");
    ASSERT(contents != NULL);

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    TspdfObj *stream = NULL;
    if (contents->type == TSPDF_OBJ_REF) {
        stream = tspdf_xref_resolve(&doc->xref, &parser, contents->ref.num, doc->obj_cache, NULL);
    } else if (contents->type == TSPDF_OBJ_STREAM) {
        stream = contents;
    } else if (contents->type == TSPDF_OBJ_ARRAY && contents->array.count > 0) {
        TspdfObj *first = &contents->array.items[0];
        if (first->type == TSPDF_OBJ_REF) {
            stream = tspdf_xref_resolve(&doc->xref, &parser, first->ref.num, doc->obj_cache, NULL);
        } else if (first->type == TSPDF_OBJ_STREAM) {
            stream = first;
        }
    }
    ASSERT(stream != NULL);
    ASSERT_EQ_INT(stream->type, TSPDF_OBJ_STREAM);

    stream->stream.data = NULL;
    stream->stream.self_contained = false;
    stream->stream.raw_offset = doc->data_len + 1;
    stream->stream.raw_len = 1;

    TspdfReader *docs[] = {doc};
    TspdfReader *merged = tspdf_reader_merge(docs, 1, &err);
    ASSERT(merged == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    tspdf_reader_destroy(doc);
}

TEST(test_extract_preserves_inherited_page_attributes) {
    size_t len = 0;
    char *pdf = make_inherited_page_attrs_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *src_page = tspdf_reader_get_page(doc, 0);
    ASSERT(src_page != NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Resources") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "MediaBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "BleedBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "TrimBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "ArtBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Rotate") == NULL);
    ASSERT_EQ_INT(src_page->rotate, 90);
    ASSERT_EQ_INT((int)src_page->media_box[2], 300);
    ASSERT_EQ_INT((int)src_page->media_box[3], 400);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(reopened, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 90);
    ASSERT_EQ_INT((int)page->media_box[2], 300);
    ASSERT_EQ_INT((int)page->media_box[3], 400);

    TspdfObj *resources = tspdf_dict_get(page->page_dict, "Resources");
    ASSERT(resources != NULL);
    ASSERT_EQ_INT(resources->type, TSPDF_OBJ_DICT);
    TspdfObj *fonts = tspdf_dict_get(resources, "Font");
    ASSERT(fonts != NULL);
    ASSERT_EQ_INT(fonts->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(fonts, "F1") != NULL);

    TspdfObj *bleed_box = tspdf_dict_get(page->page_dict, "BleedBox");
    ASSERT(bleed_box != NULL);
    ASSERT_EQ_INT(bleed_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(bleed_box->array.count, 4);
    ASSERT_EQ_INT((int)bleed_box->array.items[2].integer, 295);

    TspdfObj *trim_box = tspdf_dict_get(page->page_dict, "TrimBox");
    ASSERT(trim_box != NULL);
    ASSERT_EQ_INT(trim_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(trim_box->array.count, 4);
    ASSERT_EQ_INT((int)trim_box->array.items[2].integer, 290);

    TspdfObj *art_box = tspdf_dict_get(page->page_dict, "ArtBox");
    ASSERT(art_box != NULL);
    ASSERT_EQ_INT(art_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(art_box->array.count, 4);
    ASSERT_EQ_INT((int)art_box->array.items[2].integer, 285);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_serializes_inferred_page_type_and_parent) {
    size_t len = 0;
    char *pdf = make_missing_page_tree_type_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *src_page = tspdf_reader_get_page(doc, 0);
    ASSERT(src_page != NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Type") == NULL);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(reopened, 0);
    ASSERT(page != NULL);

    TspdfObj *type = tspdf_dict_get(page->page_dict, "Type");
    ASSERT(type != NULL);
    ASSERT_EQ_INT(type->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)type->string.data, "Page");

    TspdfObj *parent = tspdf_dict_get(page->page_dict, "Parent");
    ASSERT(parent != NULL);
    ASSERT_EQ_INT(parent->type, TSPDF_OBJ_REF);
    ASSERT_EQ_INT(parent->ref.num, 2);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_single_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 1);

    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

TEST(test_extract_single_page_omits_unselected_sibling_streams) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "(Page 2)"));
    ASSERT(!bytes_contains(out, out_len, "(Page 1)"));
    ASSERT(!bytes_contains(out, out_len, "(Page 3)"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

TEST(test_open_tspdf_one_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    // Default tspdf page size is A4: 595.276 x 841.89
    ASSERT(page->media_box[2] > 595.0 && page->media_box[2] < 596.0);
    ASSERT(page->media_box[3] > 841.0 && page->media_box[3] < 842.0);
    tspdf_reader_destroy(doc);
}

TEST(test_open_tspdf_three_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 3);
    tspdf_reader_destroy(doc);
}

TEST(test_open_v4_identity_stmf_reads_plaintext_stream) {
    size_t len = 0;
    char *pdf = make_v4_identity_stmf_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_with_password(
        (const uint8_t *)pdf, len, "", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    // Saving without encryption must surface the plaintext content stream
    // verbatim: /StmF /Identity means it was never encrypted, so decryption is
    // a no-op rather than mangling the bytes.
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "IDENTITYSTREAM"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_open_null_data) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(NULL, 0, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_open_truncated_pdf) {
    const char *truncated = "%PDF-1.4\n1 0 obj\n<<";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)truncated, strlen(truncated), &err);
    ASSERT(doc == NULL);
}

TEST(test_open_cyclic_page_tree_rejected) {
    size_t len = 0;
    char *pdf = make_cyclic_page_tree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_open_pdf_header_only_no_startxref_scan_crash) {
    static const char header_only[] = "%PDF-";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)header_only, strlen(header_only), &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);
}

TEST(test_open_pdf_rejects_overflowing_xref_subsection) {
    char pdf[256];
    int pdf_len = snprintf(pdf, sizeof(pdf),
                           "%%PDF-1.4\n"
                           "xref\n"
                           "%zu 1\n"
                           "0000000000 65535 f \n"
                           "trailer\n<< /Size 1 >>\n"
                           "startxref\n9\n%%EOF",
                           (size_t)-1);
    ASSERT(pdf_len > 0);
    ASSERT((size_t)pdf_len < sizeof(pdf));

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, (size_t)pdf_len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);
}

TEST(test_extract_out_of_bounds) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {5}; // out of bounds
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_invalid_angle) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_rotate(doc, pages, 1, 45, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
    tspdf_reader_destroy(doc);
}

TEST(test_merge_zero_docs) {
    TspdfError err;
    TspdfReader *result = tspdf_reader_merge(NULL, 0, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_open_encrypted_pdf_unsupported) {
    // Hand-crafted PDF with /Encrypt in trailer
    static const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n"
        "xref\n"
        "0 3\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "trailer\n<< /TspdfSize 3 /Root 1 0 R /Encrypt << /V 1 >> >>\n"
        "startxref\n110\n%%EOF";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_ENCRYPTED);
}

TEST(test_error_string) {
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_OK), "success");
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_PARSE), "PDF parsing failed");
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_UNSUPPORTED), "unsupported PDF feature");
}

TEST(test_full_pipeline) {
    TspdfError err;

    // Open 3-page doc
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Extract pages 0 and 2
    size_t extract[] = {0, 2};
    TspdfReader *extracted = tspdf_reader_extract(doc, extract, 2, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 2);

    // Rotate first page 90 degrees
    size_t rot_pages[] = {0};
    TspdfReader *rotated = tspdf_reader_rotate(extracted, rot_pages, 1, 90, &err);
    ASSERT(rotated != NULL);

    // Merge with original 1-page doc
    TspdfReader *one = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(one != NULL);
    TspdfReader *merge_docs[] = {rotated, one};
    TspdfReader *merged = tspdf_reader_merge(merge_docs, 2, &err);
    ASSERT(merged != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(merged), 3);

    // Save to memory, reopen, verify
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *final_doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(final_doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(final_doc), 3);
    ASSERT_EQ_INT(tspdf_reader_get_page(final_doc, 0)->rotate, 90);

    tspdf_reader_destroy(final_doc);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(one);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

TEST(test_phase3_full_pipeline) {
    TspdfError err;

    // Open 3-page PDF
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Overlay text on page 1
    TspdfWriter *res = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, font, 36.0);
    tspdf_stream_move_to(overlay, 72, 200);
    tspdf_stream_show_text(overlay, "WATERMARK");
    err = tspdf_page_end_content(doc, 0, overlay, res);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res);

    // Add annotations
    err = tspdf_page_add_link(doc, 0, 72, 700, 200, 20, "https://example.com");
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_page_add_stamp(doc, 1, 100, 400, 200, 50, "Draft");
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Set metadata
    tspdf_reader_set_title(doc, "Phase 3 Test");

    // Save encrypted
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                  "test", "test", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Reopen with password
    TspdfReader *doc2 = tspdf_reader_open_with_password(enc, enc_len, "test", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    // Save unencrypted (explicit opt-out; plain saves preserve encryption)
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc2, &out, &out_len,
                                                   &unlock_opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Final reopen
    TspdfReader *doc3 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 3);

    tspdf_reader_destroy(doc3);
    free(out);
    tspdf_reader_destroy(doc2);
    free(enc);
    tspdf_reader_destroy(doc);
}

TEST(test_large_document_500_pages) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    ASSERT(font != NULL);

    for (int i = 0; i < 500; i++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        ASSERT(page != NULL);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 12);
        tspdf_stream_move_to(page, 100, 700);
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d", i + 1);
        tspdf_stream_show_text(page, buf);
        tspdf_stream_end_text(page);
    }

    uint8_t *data = NULL;
    size_t len = 0;
    TspdfError terr = tspdf_writer_save_to_memory(doc, &data, &len);
    ASSERT(terr == TSPDF_OK);
    ASSERT(data != NULL);

    TspdfError err;
    TspdfReader *rdoc = tspdf_reader_open(data, len, &err);
    ASSERT(rdoc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(rdoc), 500);
    tspdf_reader_destroy(rdoc);

    free(data);
    tspdf_writer_destroy(doc);
}

TEST(test_error_string_page_range) {
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_PAGE_RANGE),
                  "page index out of range");
}

TEST(test_extract_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0, 7};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_delete_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {7};
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {7};
    TspdfReader *result = tspdf_reader_rotate(doc, pages, 1, 90, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_reorder_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t order[] = {2, 1, 7};
    TspdfReader *result = tspdf_reader_reorder(doc, order, 3, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_extract_drops_document_level_trees) {
    size_t len = 0;
    char *pdf = make_pinned_trees_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The document-level trees and everything only they referenced are gone
    ASSERT(!bytes_contains(out, out_len, "/StructTreeRoot"));
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "/Dests"));
    ASSERT(!bytes_contains(out, out_len, "/Names"));
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "PINNEDPAYLOAD"));
    // /PageLabels is rebuilt for the kept page (the payload dict carried no
    // /S//P//St, so the rebuilt range is a blank label) — the marker object
    // itself must still be unreachable.
    ASSERT(bytes_contains(out, out_len, "/PageLabels"));

    // Other catalog entries survive
    ASSERT(bytes_contains(out, out_len, "/Lang"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    ASSERT(tspdf_dict_get(doc2->catalog, "StructTreeRoot") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Outlines") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Lang") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_recompress_adds_flate_to_unfiltered_streams) {
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out_len < len);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    TspdfObj *filter = tspdf_dict_get(contents->stream.dict, "Filter");
    ASSERT(filter != NULL);
    ASSERT_EQ_INT(filter->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    // The compressed bytes must decode back to the original payload
    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_keeps_incompressible_stream_verbatim) {
    // Pseudo-random binary payload: deflate cannot shrink it, so the
    // keep-smaller rule must leave the stream unfiltered and untouched.
    size_t payload_len = 5000;
    char *payload = (char *)malloc(payload_len);
    ASSERT(payload != NULL);
    uint32_t state = 0x2545F491u;
    for (size_t i = 0; i < payload_len; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = (char)(state & 0xFF);
    }

    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);
    ASSERT(tspdf_dict_get(contents->stream.dict, "Filter") == NULL);
    ASSERT_EQ_SIZE(contents->stream.raw_len, payload_len);
    ASSERT(memcmp(doc2->data + contents->stream.raw_offset, payload, payload_len) == 0);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_reflates_large_flate_stream) {
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_stored_flate_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // The stored-block stream is > 10KB and highly compressible: with the old
    // 4KB skip floor removed the output must shrink substantially.
    ASSERT(out_len + 4000 < len);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_encrypted_source_roundtrip) {
    // Streams of an encrypted source are decrypted in memory before
    // recompression: the recompressed plaintext output must decode cleanly.
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *enc_doc = tspdf_reader_open_with_password(enc, enc_len, "user123", &err);
    ASSERT(enc_doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    // Saves preserve encryption by default (where recompression is skipped);
    // this test targets the decrypt+recompress combination.
    opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(enc_doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    TspdfObj *filter = tspdf_dict_get(contents->stream.dict, "Filter");
    ASSERT(filter != NULL);
    ASSERT_EQ_INT(filter->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(enc_doc);
    free(enc);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_writes_xref_stream) {
    // recompress_streams targets minimal output size, so it also writes the
    // compact xref stream instead of a classic table.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_recompress_packs_objects_into_object_streams) {
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 300);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Same options the `compress` CLI command uses.
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(out_len < len);

    // Reopen with our own reader: pages, text, metadata and every object
    // must survive the round-trip.
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Hello ObjStm") != NULL);
    const char *producer = tspdf_reader_get_producer(doc2);
    ASSERT(producer != NULL);
    ASSERT(strstr(producer, "tspdf") != NULL);

    // Every in-use object resolves, and the small objects landed in object
    // streams (type-2 xref entries).
    TspdfParser parser;
    tspdf_parser_init(&parser, doc2->data, doc2->data_len, &doc2->arena);
    size_t type2_entries = 0;
    for (uint32_t i = 1; i < (uint32_t)doc2->xref.count; i++) {
        if (!doc2->xref.entries[i].in_use) continue;
        TspdfObj *obj = tspdf_xref_resolve(&doc2->xref, &parser, i, doc2->obj_cache, NULL);
        ASSERT(obj != NULL);
        if (doc2->xref.entries[i].compressed) type2_entries++;
    }
    ASSERT(type2_entries >= 300);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_recompress_roundtrip_from_objstm_input) {
    // A document that itself came from object-stream input survives
    // recompression into fresh object streams.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_recompress_objstm_output_does_not_grow) {
    // Do-no-harm: recompressing an ObjStm-heavy file (our own compressed
    // output) must not inflate it.
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 300);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out1 = NULL;
    size_t out1_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out1, &out1_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out1, out1_len, "/Type /ObjStm"));

    TspdfReader *doc1 = tspdf_reader_open(out1, out1_len, &err);
    ASSERT(doc1 != NULL);
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc1, &out2, &out2_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out2_len <= out1_len);

    TspdfReader *doc2 = tspdf_reader_open(out2, out2_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out2);
    tspdf_reader_destroy(doc1);
    free(out1);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_save_has_no_objstm) {
    // Classic (non-ObjStm) inputs are never force-packed: an encrypted save
    // of a classic file keeps plain top-level objects and a classic xref
    // table. (ObjStm inputs DO re-pack — see the tests below.)
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 50);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Type /ObjStm"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_default_save_repacks_objstm_input) {
    // A default (non-compress) save of a file that used object streams must
    // re-pack them, not explode every member into a classic top-level object
    // (which roughly doubles ObjStm-heavy files) — and must not copy the
    // source ObjStm/XRef containers along as orphans.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    // Xref stream instead of a classic table (type-2 entries need it).
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(!bytes_contains(out, out_len, "\ntrailer"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    // No orphans: every in-use object must resolve.
    TspdfParser parser;
    tspdf_parser_init(&parser, doc2->data, doc2->data_len, &doc2->arena);
    for (uint32_t i = 1; i < (uint32_t)doc2->xref.count; i++) {
        if (!doc2->xref.entries[i].in_use) continue;
        ASSERT(tspdf_xref_resolve(&doc2->xref, &parser, i, doc2->obj_cache, NULL) != NULL);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_default_save_classic_input_stays_classic) {
    // qpdf's preserve-style rule: object streams are only written when the
    // input had them (or `compress` asks for minimal output).
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 50);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(bytes_contains(out, out_len, "\ntrailer"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_save_repacks_objstm_input) {
    // Encrypted saves re-pack ObjStm inputs too: member bodies carry plain
    // strings and the container stream is encrypted as one unit, with the
    // trailer keys (/Encrypt, /ID) on the — itself unencrypted — xref stream.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(bytes_contains(out, out_len, "/Encrypt"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    // Round-trip once more (re-save of the opened-encrypted doc exercises the
    // preserved-crypt repack path).
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out2, out2_len, "/Type /ObjStm"));
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 1);

    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_info_strings_are_encrypted) {
    // ISO 32000 exempts only the /Encrypt dict and /ID from string
    // encryption: Info metadata written into an encrypted file must be
    // encrypted with the Info object's key, or readers that decrypt it
    // unconditionally (poppler among them) show garbage.
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 10);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "SeekritTitle42");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "SeekritTitle42"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT(strcmp(title, "SeekritTitle42") == 0);

    // Same on the preserved-crypt path: edit metadata on the opened
    // encrypted document and re-save with the original encryption.
    tspdf_reader_set_title(doc2, "SecondSecret77");
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out2, out2_len, "SecondSecret77"));
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);
    const char *title3 = tspdf_reader_get_title(doc3);
    ASSERT(title3 != NULL);
    ASSERT(strcmp(title3, "SecondSecret77") == 0);

    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypt_preserves_source_info) {
    // Encrypting a file with untouched metadata must not drop its Info dict:
    // the trailer keeps /Info and the strings are encrypted like any other.
    size_t len = 0;
    char *pdf = make_titled_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Info strings must be encrypted, not stored in the clear.
    ASSERT(!bytes_contains(out, out_len, "KeepMeTitle99"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "KeepMeTitle99");
    const char *author = tspdf_reader_get_author(doc2);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Ann Author");

    // Preserved-crypt re-save (plain save of an opened encrypted document)
    // must carry the Info through as well.
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out2, out2_len, "KeepMeTitle99"));
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);
    title = tspdf_reader_get_title(doc3);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "KeepMeTitle99");

    // Decrypt round-trip: the unlocked output shows the original metadata.
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc3, &plain, &plain_len, &unlock_opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *doc4 = tspdf_reader_open(plain, plain_len, &err);
    ASSERT(doc4 != NULL);
    title = tspdf_reader_get_title(doc4);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "KeepMeTitle99");
    author = tspdf_reader_get_author(doc4);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Ann Author");

    tspdf_reader_destroy(doc4);
    free(plain);
    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_plain_save_update_producer_keeps_info) {
    // The default save (update_producer) rewrites the Info dict for the
    // Producer stamp; that must merge the original fields, not drop them.
    size_t len = 0;
    char *pdf = make_titled_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "KeepMeTitle99");
    const char *author = tspdf_reader_get_author(doc2);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Ann Author");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_plain_save_no_producer_keeps_info) {
    // With update_producer off and no metadata edits the source Info object
    // is carried over and stays referenced from the trailer.
    size_t len = 0;
    char *pdf = make_titled_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.update_producer = false;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "KeepMeTitle99");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_save_id_halves_differ) {
    // ISO 32000 7.5.5: the second /ID entry is updated when the file is
    // written, so it should differ from the first. The FIRST half is key
    // material (RC4/AESV2 key derivation) and must stay what the crypt says.
    size_t len = 0;
    char *pdf = make_titled_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    char id0[128], id1[128];
    ASSERT(find_id_halves(out, out_len, id0, id1, sizeof(id0)));
    ASSERT(strlen(id0) == 32);  // 16 random bytes, hex-encoded
    ASSERT(strlen(id1) > 0);
    ASSERT(strcmp(id0, id1) != 0);

    // Preserved-crypt re-save: the first half is key material and must stay
    // the ORIGINAL value, or the original password stops working.
    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    char rid0[128], rid1[128];
    ASSERT(find_id_halves(out2, out2_len, rid0, rid1, sizeof(rid0)));
    ASSERT_EQ_STR(rid0, id0);
    ASSERT(strcmp(rid0, rid1) != 0);

    // The re-saved file still opens with the original password.
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);

    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_delete_keeps_document_level_trees) {
    size_t len = 0;
    char *pdf = make_pinned_trees_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Delete keeps the document-level trees (only extract drops them)
    ASSERT(bytes_contains(out, out_len, "/StructTreeRoot"));
    ASSERT(bytes_contains(out, out_len, "/Outlines"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_merge_preserves_outlines) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(2, true, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, true, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);

    // Top level: AA-CH1, AA-CH2, BB-CH1, BB-CH2 in source order.
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 4);

    TspdfObj *it1 = dt_get(re, root, "First");
    ASSERT(it1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it1, "Title"), "AA-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it1), 0);

    // AA-CH1 keeps its child, pointing at merged page 1.
    TspdfObj *sub = dt_get(re, it1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "AA-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 1);

    TspdfObj *it2 = dt_get(re, it1, "Next");
    ASSERT(it2 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it2, "Title"), "AA-CH2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it2), 1);

    TspdfObj *it3 = dt_get(re, it2, "Next");
    ASSERT(it3 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it3, "Title"), "BB-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it3), 2);

    TspdfObj *it4 = dt_get(re, it3, "Next");
    ASSERT(it4 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it4, "Title"), "BB-CH2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it4), 2);
    ASSERT(tspdf_dict_get(it4, "Next") == NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_concatenates_acroform_fields) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, true, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, false, true, "BB", "Times-Roman", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);

    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 4);

    // All four fields are registered with their names intact.
    bool seen_aa_text = false, seen_aa_check = false;
    bool seen_bb_text = false, seen_bb_check = false;
    for (size_t i = 0; i < fields->array.count; i++) {
        TspdfObj *field = test_resolve_ref(re, &fields->array.items[i]);
        ASSERT(field != NULL && field->type == TSPDF_OBJ_DICT);
        TspdfObj *name = tspdf_dict_get(field, "T");
        if (dt_str_eq(name, "AA_text")) seen_aa_text = true;
        if (dt_str_eq(name, "AA_check")) seen_aa_check = true;
        if (dt_str_eq(name, "BB_text")) seen_bb_text = true;
        if (dt_str_eq(name, "BB_check")) seen_bb_check = true;
    }
    ASSERT(seen_aa_text && seen_aa_check && seen_bb_text && seen_bb_check);

    // /NeedAppearances carries through; /DR font dicts merge, first source
    // wins the F1 name collision (Helvetica, not Times-Roman).
    TspdfObj *needap = tspdf_dict_get(acro, "NeedAppearances");
    ASSERT(needap != NULL && needap->type == TSPDF_OBJ_BOOL && needap->boolean);
    TspdfObj *dr = dt_get(re, acro, "DR");
    ASSERT(dr != NULL && dr->type == TSPDF_OBJ_DICT);
    TspdfObj *dr_fonts = dt_get(re, dr, "Font");
    ASSERT(dr_fonts != NULL && dr_fonts->type == TSPDF_OBJ_DICT);
    TspdfObj *f1 = dt_get(re, dr_fonts, "F1");
    ASSERT(f1 != NULL && f1->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(f1, "BaseFont"), "Helvetica"));

    // Both widgets remain wired into their pages' /Annots.
    TspdfObj *page0_annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    TspdfObj *page1_annots = dt_get(re, re->pages.pages[1].page_dict, "Annots");
    ASSERT(page0_annots != NULL && page0_annots->type == TSPDF_OBJ_ARRAY &&
           page0_annots->array.count >= 2);
    ASSERT(page1_annots != NULL && page1_annots->type == TSPDF_OBJ_ARRAY &&
           page1_annots->array.count >= 2);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_without_doctrees_stays_clean) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_keeps_outline_subtree_for_kept_pages) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "CC", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Keep pages 0 and 1: CC-CH1 (page 0) and its child (page 1) survive,
    // CC-CH2 (page 2) is pruned.
    size_t pages[] = {0, 1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "CC-CH2"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 1);

    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(ch1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "CC-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, ch1), 0);
    ASSERT(tspdf_dict_get(ch1, "Next") == NULL);

    TspdfObj *sub = dt_get(re, ch1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "CC-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_keeps_parent_when_only_child_survives) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "DD", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Keep only page 1: DD-CH1 points at dropped page 0 but its child
    // DD-CH1-SUB points at the kept page, so the parent stays (dest dropped).
    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(ch1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "DD-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, ch1), SIZE_MAX);  // dest to dropped page gone
    TspdfObj *sub = dt_get(re, ch1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "DD-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_omits_outlines_when_nothing_survives) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "EE", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // EE bookmarks point at pages 0, 1 and 2... page 2 has EE-CH2. Build a
    // fresh 3-page source whose bookmarks all target page 0 instead.
    tspdf_reader_destroy(doc);
    free(pdf);

    TspdfWriter *w = tspdf_writer_create();
    ASSERT(w != NULL);
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    for (int i = 0; i < 3; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 14);
        tspdf_stream_move_to(page, 72, 700);
        tspdf_stream_show_text(page, "x");
        tspdf_stream_end_text(page);
    }
    tspdf_writer_add_bookmark(w, "FIRSTONLY", 0);
    uint8_t *data = NULL;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &data, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    doc = tspdf_reader_open(data, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {2};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "FIRSTONLY"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(data);
}

TEST(test_extract_prunes_acroform_fields_by_page) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, false, true, "FF", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // FF_text sits on page 0, FF_check on page 2. Keep page 0 only.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "FF_check"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);
    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 1);
    TspdfObj *field = test_resolve_ref(re, &fields->array.items[0]);
    ASSERT(field != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(field, "T"), "FF_text"));

    tspdf_reader_destroy(re);
    free(out);

    // Keeping only the middle page (no fields) omits /AcroForm entirely.
    size_t middle[] = {1};
    TspdfReader *none = tspdf_reader_extract(doc, middle, 1, &err);
    ASSERT(none != NULL);
    err = tspdf_reader_save_to_memory(none, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "FF_text"));
    free(out);
    tspdf_reader_destroy(none);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_flattens_named_dests) {
    size_t len = 0;
    char *pdf = dt_make_named_dest_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    // Keep page 0: NAMED1 (name tree -> page 0) and NAMED3 (/Dests dict ->
    // page 0) survive with explicit dest arrays; NAMED2 (GoTo -> page 1) and
    // URIITEM (no page target) are dropped.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Names"));
    ASSERT(!bytes_contains(out, out_len, "NAMED2"));
    ASSERT(!bytes_contains(out, out_len, "URIITEM"));
    ASSERT(!bytes_contains(out, out_len, "target1"));  // flattened away

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 2);

    TspdfObj *n1 = dt_get(re, root, "First");
    ASSERT(n1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(n1, "Title"), "NAMED1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n1), 0);

    TspdfObj *n3 = dt_get(re, n1, "Next");
    ASSERT(n3 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(n3, "Title"), "NAMED3"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n3), 0);
    ASSERT(tspdf_dict_get(n3, "Next") == NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_merge_flattens_named_dests_and_keeps_uri_actions) {
    size_t len_a = 0, len_b = 0;
    char *pdf_a = dt_make_named_dest_pdf(&len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, false, false, "PL", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 4);

    TspdfObj *n1 = dt_get(re, root, "First");
    ASSERT(n1 != NULL && dt_str_eq(tspdf_dict_get(n1, "Title"), "NAMED1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n1), 0);

    // The named /GoTo action is flattened to an explicit dest on page 1.
    TspdfObj *n2 = dt_get(re, n1, "Next");
    ASSERT(n2 != NULL && dt_str_eq(tspdf_dict_get(n2, "Title"), "NAMED2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n2), 1);

    TspdfObj *n3 = dt_get(re, n2, "Next");
    ASSERT(n3 != NULL && dt_str_eq(tspdf_dict_get(n3, "Title"), "NAMED3"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n3), 0);

    // The URI item survives a merge with its action untouched.
    TspdfObj *n4 = dt_get(re, n3, "Next");
    ASSERT(n4 != NULL && dt_str_eq(tspdf_dict_get(n4, "Title"), "URIITEM"));
    TspdfObj *action = dt_get(re, n4, "A");
    ASSERT(action != NULL && action->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(action, "S"), "URI"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_prunes_field_kids_and_carries_da) {
    size_t len = 0;
    char *pdf = dt_make_field_kids_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 3);

    // Keep page 1: the parent field survives with only the second widget
    // (whose page is only discoverable through /Annots, it has no /P).
    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(acro, "DA"), "/Helv 0 Tf 0 g"));
    TspdfObj *needap = tspdf_dict_get(acro, "NeedAppearances");
    ASSERT(needap != NULL && needap->type == TSPDF_OBJ_BOOL && needap->boolean);

    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 1);
    TspdfObj *parent = test_resolve_ref(re, &fields->array.items[0]);
    ASSERT(parent != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(parent, "T"), "parentfield"));
    TspdfObj *kids = dt_get(re, parent, "Kids");
    ASSERT(kids != NULL && kids->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(kids->array.count, 1);
    TspdfObj *kid = test_resolve_ref(re, &kids->array.items[0]);
    ASSERT(kid != NULL && kid->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(kid, "Subtype"), "Widget"));

    // The kept widget stays in its page's /Annots.
    TspdfObj *annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    ASSERT(annots != NULL && annots->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(annots->array.count, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);

    // Keep page 2 (no widgets anywhere): the whole field, and with it the
    // /AcroForm, disappears.
    size_t last[] = {2};
    TspdfReader *none = tspdf_reader_extract(doc, last, 1, &err);
    ASSERT(none != NULL);
    err = tspdf_reader_save_to_memory(none, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "parentfield"));
    free(out);
    tspdf_reader_destroy(none);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_and_merge_bounded_on_cyclic_nametree) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_nametree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "CYCITEM"));
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    TspdfReader *doc_b = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);
    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "CYCITEM"));
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf);
}

TEST(test_extract_bounded_on_cyclic_field_kids) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_field_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "loopfield"));

    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_breaks_cyclic_outline_siblings) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_outline_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 2);

    TspdfObj *a = dt_get(re, root, "First");
    ASSERT(a != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(a, "Title"), "LOOPA"));
    ASSERT(tspdf_dict_get(a, "Prev") == NULL);
    TspdfObj *b = dt_get(re, a, "Next");
    ASSERT(b != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(b, "Title"), "LOOPB"));
    ASSERT(tspdf_dict_get(b, "Next") == NULL);
    TspdfObj *b_prev = dt_get(re, b, "Prev");
    ASSERT(b_prev != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(b_prev, "Title"), "LOOPA"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_attach_add_save_reopen_roundtrip) {
    uint8_t *buf = NULL;
    TspdfReader *re = att_add_and_reopen("hello.txt", att_payload,
                                         sizeof(att_payload), "greeting", &buf);
    ASSERT(re != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_attachments(re, &infos, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT(infos != NULL);
    ASSERT_EQ_STR(infos[0].name, "hello.txt");
    ASSERT_EQ_SIZE(infos[0].size, sizeof(att_payload));
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, "greeting");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    err = tspdf_reader_attachment_get(re, "hello.txt", &bytes, &blen);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    // Unknown names report NOT_FOUND, not a parse error.
    err = tspdf_reader_attachment_get(re, "nope.txt", &bytes, &blen);
    ASSERT_EQ_INT(err, TSPDF_ERR_NOT_FOUND);

    tspdf_reader_destroy(re);
    free(buf);
}

TEST(test_attach_add_ex_params_subtype_checksum) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AX", "Helvetica", &src_len);
    ASSERT(src != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);

    // 1700000000 = 2023-11-14 22:13:20 UTC.
    err = tspdf_reader_attachment_add_ex(doc, "notes.txt", att_payload,
                                         sizeof(att_payload), "notes",
                                         "text/plain", 1700000000LL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    // The enumerator reports the MIME type and decoded size.
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT(infos[0].mime != NULL);
    ASSERT_EQ_STR(infos[0].mime, "text/plain");
    ASSERT_EQ_SIZE(infos[0].size, sizeof(att_payload));

    // Raw structure: /Names -> /EmbeddedFiles -> Filespec -> /EF /F stream.
    TspdfObj *names = dt_catalog_get(re, "Names");
    ASSERT(names && names->type == TSPDF_OBJ_DICT);
    TspdfObj *ef_root = dt_get(re, names, "EmbeddedFiles");
    ASSERT(ef_root && ef_root->type == TSPDF_OBJ_DICT);
    TspdfObj *pairs = dt_get(re, ef_root, "Names");
    ASSERT(pairs && pairs->type == TSPDF_OBJ_ARRAY && pairs->array.count == 2);
    TspdfObj *fs = test_resolve_ref(re, &pairs->array.items[1]);
    ASSERT(fs && fs->type == TSPDF_OBJ_DICT);
    TspdfObj *ef = dt_get(re, fs, "EF");
    ASSERT(ef && ef->type == TSPDF_OBJ_DICT);
    TspdfObj *stream = test_resolve_ref(re, tspdf_dict_get(ef, "F"));
    ASSERT(stream && stream->type == TSPDF_OBJ_STREAM);

    ASSERT(dt_str_eq(tspdf_dict_get(stream->stream.dict, "Subtype"), "text/plain"));
    TspdfObj *params = test_resolve_ref(re, tspdf_dict_get(stream->stream.dict,
                                                           "Params"));
    ASSERT(params && params->type == TSPDF_OBJ_DICT);
    TspdfObj *size = tspdf_dict_get(params, "Size");
    ASSERT(size && size->type == TSPDF_OBJ_INT);
    ASSERT_EQ_SIZE((size_t)size->integer, sizeof(att_payload));
    ASSERT(dt_str_eq(tspdf_dict_get(params, "ModDate"), "D:20231114221320Z"));

    uint8_t digest[16];
    md5_hash(att_payload, sizeof(att_payload), digest);
    TspdfObj *cks = tspdf_dict_get(params, "CheckSum");
    ASSERT(cks && cks->type == TSPDF_OBJ_STRING && cks->string.len == 16);
    ASSERT(memcmp(cks->string.data, digest, 16) == 0);

    // The bytes still round-trip.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "notes.txt", &bytes, &blen),
                  TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_flat_tree_keys_sorted) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    // Insert out of order; the flat node must come out sorted.
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "m.bin", att_payload, 4, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "a.bin", att_payload, 3, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "z.bin", att_payload, 5, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "ab.bin", att_payload, 2, NULL), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 4);
    // Enumeration follows tree order, so this asserts the stored key order
    // ("a" < "ab": shorter prefix sorts first).
    ASSERT_EQ_STR(infos[0].name, "a.bin");
    ASSERT_EQ_STR(infos[1].name, "ab.bin");
    ASSERT_EQ_STR(infos[2].name, "m.bin");
    ASSERT_EQ_STR(infos[3].name, "z.bin");
    // Sizes tell the entries apart, proving values track their keys.
    ASSERT_EQ_SIZE(infos[0].size, 3);
    ASSERT_EQ_SIZE(infos[1].size, 2);
    ASSERT_EQ_SIZE(infos[2].size, 4);
    ASSERT_EQ_SIZE(infos[3].size, 5);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_add_replaces_same_name) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "f.txt",
                  (const uint8_t *)"old", 3, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "f.txt",
                  (const uint8_t *)"newer", 5, NULL), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "f.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 5);
    ASSERT(memcmp(bytes, "newer", 5) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_remove) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "keep.txt",
                  (const uint8_t *)"keep", 4, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "drop.txt",
                  (const uint8_t *)"drop", 4, NULL), TSPDF_OK);

    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "missing.txt"),
                  TSPDF_ERR_NOT_FOUND);
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "drop.txt"), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "keep.txt");
    // The removed attachment's objects must not linger in the output.
    ASSERT(!bytes_contains(out, out_len, "drop.txt"));
    tspdf_reader_destroy(re);
    free(out);

    // Removing the last attachment removes the whole /EmbeddedFiles tree
    // (and the then-empty /Names dict).
    src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);
    doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "only.txt",
                  (const uint8_t *)"x", 1, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "only.txt"), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/EmbeddedFiles"));
    tspdf_reader_destroy(doc);
    free(src);
    free(out);
}

TEST(test_attach_cyclic_embeddedfiles_tree_bounded) {
    size_t len = 0;
    char *pdf = att_make_cyclic_embeddedfiles_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Enumeration terminates and reports the entry exactly once.
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "cyc.txt");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "cyc.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 9);
    ASSERT(memcmp(bytes, "CYCBYTES!", 9) == 0);
    free(bytes);

    // Extract (which re-adds all attachments) is bounded too, and the
    // rebuilt flat tree carries the entry through.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "cyc.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 9);
    ASSERT(memcmp(bytes, "CYCBYTES!", 9) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_attach_list_size_ignores_lying_params) {
    // /Params /Size is unverified input; the listing must report the real
    // decoded payload length when the stream decodes.
    size_t len = 0;
    char *pdf = att_make_params_pdf(&len, "/Params << /Size 999999 >>");
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "liar.bin");
    ASSERT_EQ_SIZE(infos[0].size, 9);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_attach_list_size_params_fallback_when_undecodable) {
    // When the stream cannot be decoded (unsupported filter) the /Params
    // /Size claim is still better than nothing.
    size_t len = 0;
    char *pdf = att_make_params_pdf(&len,
                                    "/Filter /JBIG2Decode /Params << /Size 12345 >>");
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_SIZE(infos[0].size, 12345);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_attach_extract_keeps_all_attachments) {
    uint8_t *buf = NULL;
    TspdfReader *src = att_add_and_reopen("data.bin", att_payload,
                                          sizeof(att_payload), "desc here", &buf);
    ASSERT(src != NULL);

    // Extract only page 1 of 2: attachments are document-level and all survive.
    size_t pages[] = {1};
    TspdfError err;
    TspdfReader *result = tspdf_reader_extract(src, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "data.bin");
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, "desc here");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "data.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(src);
    free(buf);
}

TEST(test_attach_merge_unions_first_wins) {
    uint8_t *buf_a = NULL;
    uint8_t *buf_b = NULL;
    TspdfReader *doc_a = att_add_and_reopen("shared.txt",
                                            (const uint8_t *)"from A", 6,
                                            NULL, &buf_a);
    TspdfReader *doc_b = att_add_and_reopen("shared.txt",
                                            (const uint8_t *)"from B!!", 8,
                                            NULL, &buf_b);
    ASSERT(doc_a != NULL && doc_b != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc_a, "a.txt",
                  (const uint8_t *)"AA", 2, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc_b, "b.txt",
                  (const uint8_t *)"BB", 2, NULL), TSPDF_OK);

    TspdfError err;
    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(merged, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 3);
    ASSERT_EQ_STR(infos[0].name, "a.txt");
    ASSERT_EQ_STR(infos[1].name, "b.txt");
    ASSERT_EQ_STR(infos[2].name, "shared.txt");

    // First source wins the name collision.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "shared.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 6);
    ASSERT(memcmp(bytes, "from A", 6) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(buf_a);
    free(buf_b);
}

TEST(test_attach_delete_pages_keeps_attachments) {
    uint8_t *buf = NULL;
    TspdfReader *src = att_add_and_reopen("data.bin", att_payload,
                                          sizeof(att_payload), NULL, &buf);
    ASSERT(src != NULL);

    size_t pages[] = {0};
    TspdfError err;
    TspdfReader *result = tspdf_reader_delete(src, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "data.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(src);
    free(buf);
}

TEST(test_attach_encrypted_roundtrip) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "secret.bin", att_payload,
                                              sizeof(att_payload), NULL), TSPDF_OK);

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                        "pw", "pw", 0xFFFFFFFF, 256),
                  TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    // Wrong/missing password never reaches the attachment.
    TspdfReader *no_pw = tspdf_reader_open(enc, enc_len, &err);
    ASSERT(no_pw == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_ENCRYPTED);

    // With the password the attachment stream decrypts and decodes.
    TspdfReader *re = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(re != NULL);
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "secret.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    // And an extract of the encrypted document still carries it. The extract
    // inherits the source's crypt, so its save stays encrypted under the
    // same password.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(re, pages, 1, &err);
    ASSERT(result != NULL);
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);
    TspdfReader *re2 = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(re2 != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re2, "secret.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(re);
    free(enc);
}

TEST(test_attach_unicode_name_stored_as_utf16_roundtrip) {
    // A non-ASCII attachment name is a PDF text string: it must be written
    // as UTF-16BE with BOM (ISO 32000 §7.9.2.2) in /F, /UF and the
    // EmbeddedFiles name-tree key — not as raw UTF-8 bytes.
    const char *name = "sp \xC3\xBCn\xC3\xAF" "code.txt";  // "sp ünïcode.txt"
    const char *desc = "d\xC3\xA9sc";                      // "désc"

    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, name, att_payload,
                                              sizeof(att_payload), desc), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    // Raw bytes: the escaped UTF-16 BOM (\376\377) is present and no raw
    // UTF-8 tail leaked into any string.
    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    ASSERT(!bytes_contains(out, out_len, "\xC3\xBCn\xC3\xAF"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    // Listing decodes the stored name and description back to UTF-8.
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, name);
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, desc);

    // Lookup, replace and remove all accept the UTF-8 name.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, name, &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    ASSERT_EQ_INT(tspdf_reader_attachment_add(re, name,
                                              (const uint8_t *)"newer", 5, NULL),
                  TSPDF_OK);
    TspdfAttachmentInfo *infos2 = NULL;
    size_t count2 = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos2, &count2), TSPDF_OK);
    ASSERT_EQ_SIZE(count2, 1);  // replaced, not duplicated

    ASSERT_EQ_INT(tspdf_reader_attachment_remove(re, name), TSPDF_OK);
    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_reads_foreign_encoded_names) {
    size_t len = 0;
    char *pdf = att_make_foreign_names_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Listing decodes both encodings to UTF-8 (tree order: PDFDoc key first).
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 2);
    ASSERT_EQ_STR(infos[0].name, "caf\xC3\xA9.txt");
    ASSERT_EQ_STR(infos[1].name, "\xC3\xBCn\xC3\xAF" "code.txt");

    // Lookup by the decoded UTF-8 name works for both.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "\xC3\xBCn\xC3\xAF" "code.txt",
                                              &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 11);
    ASSERT(memcmp(bytes, "hello utf16", 11) == 0);
    free(bytes);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "caf\xC3\xA9.txt",
                                              &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 12);
    ASSERT(memcmp(bytes, "hello pdfdoc", 12) == 0);
    free(bytes);

    // Adding under the same UTF-8 name replaces the PDFDocEncoded entry
    // instead of creating a differently-encoded duplicate.
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "caf\xC3\xA9.txt",
                                              (const uint8_t *)"newer", 5, NULL),
                  TSPDF_OK);
    TspdfAttachmentInfo *infos2 = NULL;
    size_t count2 = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos2, &count2), TSPDF_OK);
    ASSERT_EQ_SIZE(count2, 2);

    // Removal by the decoded name works too.
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "\xC3\xBCn\xC3\xAF" "code.txt"),
                  TSPDF_OK);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_preserves_page_labels) {
    size_t len = 0;
    char *pdf = pl_make_labeled_pdf(false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 5);

    // Keep pages 3-4 (labels iii and 1): the rebuilt tree must pin the
    // roman range at value 3 and restart decimal at the second page.
    size_t pages[] = {2, 3};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(nums != NULL && nums->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(nums->array.count, 4);
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 3));  // page 0: iii
    ASSERT(pl_expect_range(re, nums, 1, 1, 'D', 1));  // page 1: 1

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_merge_page_labels_labeled_plus_unlabeled) {
    size_t len_a = 0, len_b = 0;
    char *pdf_a = pl_make_labeled_pdf(false, &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 7);

    // i, ii, iii, 1, 2 from the labeled source; the unlabeled source keeps
    // its default decimal-from-1 numbering via an explicit range.
    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(nums != NULL && nums->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(nums->array.count, 6);
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 1));
    ASSERT(pl_expect_range(re, nums, 1, 3, 'D', 1));
    ASSERT(pl_expect_range(re, nums, 2, 5, 'D', 1));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_without_page_labels_emits_none) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/PageLabels"));

    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_bounded_on_cyclic_pagelabel_tree) {
    size_t len = 0;
    char *pdf = pl_make_labeled_pdf(true, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // The number tree's /Kids loops back on itself: the budgeted walk must
    // terminate and still pick up the /Nums entries it saw on the way.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 1));  // page 0 keeps label i

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

void run_docops_tests(void) {
    printf("\n  Manipulation:\n");
    RUN(test_extract_pages);
    RUN(test_delete_pages);
    RUN(test_rotate_pages);
    RUN(test_rotate_pages_normalizes_negative_angle);
    RUN(test_set_cropbox_explicit_box);
    RUN(test_set_cropbox_clamps_to_mediabox);
    RUN(test_set_cropbox_degenerate_rejected);
    RUN(test_set_cropbox_out_of_range);
    RUN(test_scale_factor_half);
    RUN(test_scale_nonuniform_and_cropbox);
    RUN(test_resize_to_a4_from_letter);
    RUN(test_resize_to_a4_rotate90_viewed_portrait);
    RUN(test_resize_to_a4_rotate270_viewed_portrait);
    RUN(test_resize_to_a4_rotate90_preserves_viewed_landscape);
    RUN(test_resize_to_a4_landscape_page_stays_landscape);
    RUN(test_page_crop_box_exposed);
    RUN(test_resize_to_rejects_nonpositive);
    RUN(test_scale_rejects_nonpositive_factor);
    RUN(test_reorder_pages);
    RUN(test_merge_documents);
    RUN(test_save_rejects_missing_stream_source);
    RUN(test_merge_rejects_invalid_source_stream_range);
    RUN(test_extract_preserves_inherited_page_attributes);
    RUN(test_extract_serializes_inferred_page_type_and_parent);
    RUN(test_extract_single_page);
    RUN(test_extract_single_page_omits_unselected_sibling_streams);
    printf("\n  Real PDF files:\n");
    RUN(test_open_tspdf_one_page);
    RUN(test_open_tspdf_three_pages);
    printf("\n  Error handling:\n");
    RUN(test_open_null_data);
    RUN(test_open_truncated_pdf);
    RUN(test_open_cyclic_page_tree_rejected);
    RUN(test_open_pdf_header_only_no_startxref_scan_crash);
    RUN(test_open_pdf_rejects_overflowing_xref_subsection);
    RUN(test_extract_out_of_bounds);
    RUN(test_rotate_invalid_angle);
    RUN(test_merge_zero_docs);
    RUN(test_open_v4_identity_stmf_reads_plaintext_stream);
    RUN(test_open_encrypted_pdf_unsupported);
    RUN(test_error_string);
    printf("\n  Integration:\n");
    RUN(test_full_pipeline);
    printf("\n  Phase 3 integration:\n");
    RUN(test_phase3_full_pipeline);
    printf("\n  Large document:\n");
    RUN(test_large_document_500_pages);
    printf("\n  Audit fixes (reader-core):\n");
    RUN(test_error_string_page_range);
    RUN(test_extract_out_of_range_sets_page_range_error);
    RUN(test_delete_out_of_range_sets_page_range_error);
    RUN(test_rotate_out_of_range_sets_page_range_error);
    RUN(test_reorder_out_of_range_sets_page_range_error);
    RUN(test_extract_drops_document_level_trees);
    RUN(test_delete_keeps_document_level_trees);
    RUN(test_recompress_adds_flate_to_unfiltered_streams);
    RUN(test_recompress_keeps_incompressible_stream_verbatim);
    RUN(test_recompress_reflates_large_flate_stream);
    RUN(test_recompress_encrypted_source_roundtrip);
    RUN(test_recompress_writes_xref_stream);
    RUN(test_recompress_packs_objects_into_object_streams);
    RUN(test_recompress_roundtrip_from_objstm_input);
    RUN(test_recompress_objstm_output_does_not_grow);
    RUN(test_encrypted_save_has_no_objstm);
    RUN(test_default_save_repacks_objstm_input);
    RUN(test_default_save_classic_input_stays_classic);
    RUN(test_encrypted_save_repacks_objstm_input);
    RUN(test_encrypted_info_strings_are_encrypted);
    RUN(test_encrypt_preserves_source_info);
    RUN(test_plain_save_update_producer_keeps_info);
    RUN(test_plain_save_no_producer_keeps_info);
    RUN(test_encrypted_save_id_halves_differ);
    printf("\n  Doc trees (outlines/AcroForm across merge/extract):\n");
    RUN(test_merge_preserves_outlines);
    RUN(test_merge_concatenates_acroform_fields);
    RUN(test_merge_without_doctrees_stays_clean);
    RUN(test_extract_keeps_outline_subtree_for_kept_pages);
    RUN(test_extract_keeps_parent_when_only_child_survives);
    RUN(test_extract_omits_outlines_when_nothing_survives);
    RUN(test_extract_prunes_acroform_fields_by_page);
    RUN(test_extract_flattens_named_dests);
    RUN(test_merge_flattens_named_dests_and_keeps_uri_actions);
    RUN(test_extract_prunes_field_kids_and_carries_da);
    RUN(test_extract_and_merge_bounded_on_cyclic_nametree);
    RUN(test_extract_preserves_page_labels);
    RUN(test_merge_page_labels_labeled_plus_unlabeled);
    RUN(test_merge_without_page_labels_emits_none);
    RUN(test_extract_bounded_on_cyclic_pagelabel_tree);
    RUN(test_extract_bounded_on_cyclic_field_kids);
    RUN(test_extract_breaks_cyclic_outline_siblings);
    printf("\n  Embedded file attachments:\n");
    RUN(test_attach_add_save_reopen_roundtrip);
    RUN(test_attach_add_ex_params_subtype_checksum);
    RUN(test_attach_flat_tree_keys_sorted);
    RUN(test_attach_add_replaces_same_name);
    RUN(test_attach_remove);
    RUN(test_attach_cyclic_embeddedfiles_tree_bounded);
    RUN(test_attach_list_size_ignores_lying_params);
    RUN(test_attach_list_size_params_fallback_when_undecodable);
    RUN(test_attach_extract_keeps_all_attachments);
    RUN(test_attach_merge_unions_first_wins);
    RUN(test_attach_delete_pages_keeps_attachments);
    RUN(test_attach_encrypted_roundtrip);
    RUN(test_attach_unicode_name_stored_as_utf16_roundtrip);
    RUN(test_attach_reads_foreign_encoded_names);
}
