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

static const char *MINI_PDF =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n"
    "xref\n"
    "0 4\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "trailer\n<< /TspdfSize 4 /Root 1 0 R >>\n"
    "startxref\n186\n%%EOF";

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

static char *make_openable_mini_pdf_with_startxref_adjust(const char *prefix,
                                                          size_t trailing_spaces,
                                                          int startxref_adjust,
                                                          size_t *out_len) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t cap = prefix_len + trailing_spaces + 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (prefix && !appendf(pdf, cap, &pos, "%s", prefix)) goto fail;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    size_t startxref_value = xref;
    if (startxref_adjust < 0) {
        size_t adjust = (size_t)(-startxref_adjust);
        if (adjust > startxref_value) goto fail;
        startxref_value -= adjust;
    } else {
        startxref_value += (size_t)startxref_adjust;
    }

    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, startxref_value)) goto fail;

    if (trailing_spaces > 0) {
        if (pos + trailing_spaces > cap) goto fail;
        memset(pdf + pos, ' ', trailing_spaces);
        pos += trailing_spaces;
    }

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_openable_mini_pdf(const char *prefix, size_t trailing_spaces, size_t *out_len) {
    return make_openable_mini_pdf_with_startxref_adjust(prefix, trailing_spaces, 0, out_len);
}

static char *make_prefixed_header_relative_xref_pdf(const char *prefix, size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    size_t prefix_len = prefix ? strlen(prefix) : 0;
    if (base_len > SIZE_MAX - prefix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(prefix_len + base_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    if (prefix_len > 0) {
        memcpy(pdf, prefix, prefix_len);
    }
    memcpy(pdf + prefix_len, base, base_len);
    free(base);

    *out_len = prefix_len + base_len;
    return pdf;
}

static char *make_large_prefixed_pdf(size_t prefix_len, size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    if (base_len > SIZE_MAX - prefix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(prefix_len + base_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    // Fill the prefix with a repeating byte pattern that avoids '%'. This keeps
    // the filler free of any accidental "%PDF-" occurrence.
    for (size_t i = 0; i < prefix_len; i++) {
        pdf[i] = (char)('A' + (int)(i % 26));
    }
    memcpy(pdf + prefix_len, base, base_len);
    free(base);

    *out_len = prefix_len + base_len;
    return pdf;
}

static char *make_pdf_with_bad_appended_startxref(size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    const char *suffix =
        "\n% appended malformed update\n"
        "startxref\n"
        "999999999\n"
        "%%EOF";
    size_t suffix_len = strlen(suffix);
    if (base_len > SIZE_MAX - suffix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(base_len + suffix_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    memcpy(pdf, base, base_len);
    memcpy(pdf + base_len, suffix, suffix_len);
    free(base);

    *out_len = base_len + suffix_len;
    return pdf;
}

static char *make_missing_startxref_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "%%%%EOF",
                 obj1, obj2, obj3)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_no_trailer_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos, "%%%%EOF")) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_object_scan_only_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Page /Parent 3 0 R /MediaBox [0 0 200 300] >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Catalog /Pages 3 0 R >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Pages /Kids [1 0 R] /Count 1 >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos, "%%%%EOF")) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_classic_xref_horizontal_whitespace_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0\t4\f\n"
                 "0000000000\t65535\ff\t\n"
                 "%010zu\t00000\fn\t\n"
                 "%010zu\t00000\fn\t\n"
                 "%010zu\t00000\fn\t\n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_trailer_root_value_pdf(const char *root_value, size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Pages /Kids [2 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Page /Parent 1 0 R /MediaBox [0 0 450 650] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 3\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 3 /Root %s >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, root_value, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_wrong_xref_identity_pdf(size_t *out_len,
                                          const char *root_object_header,
                                          const char *root_xref_gen,
                                          const char *root_ref_gen) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "%s\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
                 root_object_header)) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu %s n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 %s R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, root_xref_gen, obj2, obj3, root_ref_gen, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_malformed_classic_xref_pdf(const char *subsection_header,
                                             const char *entry_lines,
                                             size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "%s\n"
                 "%s"
                 "trailer\n<< /Size 1 >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 subsection_header, entry_lines ? entry_lines : "", xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
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

static void write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}

static void xref_entry9(uint8_t *dst, uint8_t type, uint32_t field1, uint32_t field2) {
    dst[0] = type;
    write_be32(dst + 1, field1);
    write_be32(dst + 5, field2);
}

static void xref_objstm_entry9(uint8_t *dst, uint32_t stream_obj, uint32_t index) {
    xref_entry9(dst, 2, stream_obj, index);
}

static char *make_recursive_compressed_xref_pdf(bool two_object_cycle, size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    uint32_t xref_obj_num = two_object_cycle ? 3 : 2;
    size_t object_count = two_object_cycle ? 4 : 3;
    size_t xref_obj = pos;
    uint8_t entries[4][4] = {{0}};
    xref_entry4(entries[0], 0, 0, 255);
    if (two_object_cycle) {
        xref_objstm_entry4(entries[1], 2, 0);
        xref_objstm_entry4(entries[2], 1, 0);
        xref_entry4(entries[3], 1, xref_obj, 0);
    } else {
        xref_objstm_entry4(entries[1], 1, 0);
        xref_entry4(entries[2], 1, xref_obj, 0);
    }

    size_t entries_len = object_count * sizeof(entries[0]);
    if (!appendf(pdf, 2048, &pos,
                 "%u 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 %zu] "
                 "/Size %zu /Root 1 0 R >>\n"
                 "stream\n",
                 xref_obj_num, entries_len, object_count, object_count)) goto fail;
    if (pos + entries_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, entries, entries_len);
    pos += entries_len;
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
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

static bool lzw_test_ensure_capacity(uint8_t **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return true;

    size_t new_cap = *cap ? *cap : 256;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = (uint8_t *)realloc(*buf, new_cap);
    if (!new_buf) return false;
    memset(new_buf + *cap, 0, new_cap - *cap);
    *buf = new_buf;
    *cap = new_cap;
    return true;
}

static bool lzw_test_write_code(uint8_t **buf, size_t *cap, size_t *bit_pos,
                                uint16_t code, int code_size) {
    if (code_size <= 0 || code_size > 12) return false;

    size_t needed_bits = *bit_pos + (size_t)code_size;
    if (needed_bits < *bit_pos) return false;
    size_t needed_bytes = (needed_bits + 7) / 8;
    if (!lzw_test_ensure_capacity(buf, cap, needed_bytes)) return false;

    for (int i = code_size - 1; i >= 0; i--) {
        if ((code >> i) & 1u) {
            size_t bit = *bit_pos;
            (*buf)[bit / 8] |= (uint8_t)(1u << (7 - (bit % 8)));
        }
        (*bit_pos)++;
    }

    return true;
}

static int lzw_test_find_code(const uint16_t *prefix, const uint8_t *suffix,
                              size_t next_code, uint16_t parent, uint8_t ch) {
    for (size_t code = 258; code < next_code; code++) {
        if (prefix[code] == parent && suffix[code] == ch) {
            return (int)code;
        }
    }
    return -1;
}

static void lzw_test_maybe_grow_code_size(size_t next_code, int *code_size) {
    if (*code_size < 12 && next_code + 1 >= ((size_t)1 << *code_size)) {
        (*code_size)++;
    }
}

static uint8_t *lzw_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t cap = 256;
    uint8_t *out = (uint8_t *)calloc(cap, 1);
    if (!out) return NULL;

    uint16_t prefix[4096] = {0};
    uint8_t suffix[4096] = {0};
    size_t next_code = 258;
    int code_size = 9;
    size_t bit_pos = 0;

    if (!lzw_test_write_code(&out, &cap, &bit_pos, 256, code_size)) goto fail;

    if (len > 0) {
        uint16_t current = data[0];
        for (size_t i = 1; i < len; i++) {
            uint8_t ch = data[i];
            int existing = lzw_test_find_code(prefix, suffix, next_code, current, ch);
            if (existing >= 0) {
                current = (uint16_t)existing;
                continue;
            }

            if (!lzw_test_write_code(&out, &cap, &bit_pos, current, code_size)) goto fail;

            if (next_code < 4096) {
                prefix[next_code] = current;
                suffix[next_code] = ch;
                next_code++;
                lzw_test_maybe_grow_code_size(next_code, &code_size);
            }

            current = ch;
        }

        if (!lzw_test_write_code(&out, &cap, &bit_pos, current, code_size)) goto fail;
    }

    if (!lzw_test_write_code(&out, &cap, &bit_pos, 257, code_size)) goto fail;

    *out_len = (bit_pos + 7) / 8;
    return out;

fail:
    free(out);
    return NULL;
}

static char *make_xref_stream_predictor_pdf(size_t *out_len,
                                            bool array_filter,
                                            bool ascii_hex_filter,
                                            bool ascii85_filter,
                                            bool run_length_filter,
                                            bool omit_type,
                                            bool direct_chain_decode_params,
                                            bool abbreviated_decode_params) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

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
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t rows[5][4];
    xref_entry4(rows[0], 0, 0, 255);
    xref_entry4(rows[1], 1, obj1, 0);
    xref_entry4(rows[2], 1, obj2, 0);
    xref_entry4(rows[3], 1, obj3, 0);
    xref_entry4(rows[4], 1, xref_obj, 0);

    uint8_t predicted[5 * 5];
    size_t pred_pos = 0;
    for (size_t r = 0; r < 5; r++) {
        predicted[pred_pos++] = 2; // PNG Up predictor
        for (size_t c = 0; c < 4; c++) {
            uint8_t prev = r > 0 ? rows[r - 1][c] : 0;
            predicted[pred_pos++] = (uint8_t)(rows[r][c] - prev);
        }
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(predicted, pred_pos, &comp_len);
    if (!comp) goto fail;

    uint8_t *encoded = comp;
    size_t encoded_len = comp_len;
    if (ascii_hex_filter) {
        encoded = ascii_hex_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    } else if (ascii85_filter) {
        encoded = ascii85_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    } else if (run_length_filter) {
        encoded = run_length_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    }

    const char *xref_type = omit_type ? "" : "/Type /XRef ";
    const char *decode_params_key = abbreviated_decode_params ? "DP" : "DecodeParms";
    const char *chain_decode_params = direct_chain_decode_params ?
                                      "<< /Columns 4 /Predictor 12 >>" :
                                      "[null << /Columns 4 /Predictor 12 >>]";

    if ((ascii_hex_filter || ascii85_filter || run_length_filter) &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter [/%s /FlateDecode] "
                 "/%s %s "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, encoded_len, run_length_filter ? "RunLengthDecode" :
                 ascii85_filter ? "ASCII85Decode" : "ASCIIHexDecode",
                 decode_params_key, chain_decode_params)) {
        if (encoded != comp) free(encoded);
        free(comp);
        goto fail;
    }

    if (!ascii_hex_filter && !ascii85_filter && !run_length_filter && !array_filter &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter /FlateDecode "
                 "/%s << /Columns 4 /Predictor 12 >> "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, comp_len, decode_params_key)) {
        free(comp);
        goto fail;
    }

    if (!ascii_hex_filter && !ascii85_filter && !run_length_filter && array_filter &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter [/FlateDecode] "
                 "/%s [<< /Columns 4 /Predictor 12 >>] "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, comp_len, decode_params_key)) {
        free(comp);
        goto fail;
    }

    if (pos + encoded_len + 128 > 4096) {
        if (encoded != comp) free(encoded);
        free(comp);
        goto fail;
    }
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    if (encoded != comp) free(encoded);
    free(comp);

    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_xref_stream_lzw_predictor_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    uint8_t *encoded = NULL;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t rows[5][4];
    xref_entry4(rows[0], 0, 0, 255);
    xref_entry4(rows[1], 1, obj1, 0);
    xref_entry4(rows[2], 1, obj2, 0);
    xref_entry4(rows[3], 1, obj3, 0);
    xref_entry4(rows[4], 1, xref_obj, 0);

    uint8_t predicted[5 * 5];
    size_t pred_pos = 0;
    for (size_t r = 0; r < 5; r++) {
        predicted[pred_pos++] = 2;
        for (size_t c = 0; c < 4; c++) {
            uint8_t prev = r > 0 ? rows[r - 1][c] : 0;
            predicted[pred_pos++] = (uint8_t)(rows[r][c] - prev);
        }
    }

    size_t encoded_len = 0;
    encoded = lzw_encode(predicted, pred_pos, &encoded_len);
    if (!encoded) goto fail;

    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /XRef /Length %zu /Filter /LZWDecode "
                 "/DecodeParms << /Columns 4 /Predictor 12 /EarlyChange 1 >> "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 encoded_len)) goto fail;
    if (pos + encoded_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    free(encoded);
    encoded = NULL;

    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(encoded);
    free(pdf);
    return NULL;
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

static char *make_object_stream_zero_first_header_spill_pdf(size_t *out_len) {
    const char *payload = "2 0 << /Type /Pages /Kids [] /Count 0 >>";
    size_t payload_len = strlen(payload);
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 1 /First 0 /Length %zu >>\n"
                 "stream\n",
                 payload_len)) goto fail;
    if (pos + payload_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, payload, payload_len);
    pos += payload_len;
    if (!appendf(pdf, 2048, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t entries[6][4];
    xref_entry4(entries[0], 0, 0, 255);
    xref_entry4(entries[1], 0, 0, 0);
    xref_objstm_entry4(entries[2], 4, 0);
    xref_entry4(entries[3], 0, 0, 0);
    xref_entry4(entries[4], 1, obj4, 0);
    xref_entry4(entries[5], 1, xref_obj, 0);

    if (!appendf(pdf, 2048, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 6] /Size 6 /Root 2 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > 2048) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_object_stream_high_index_pdf(size_t *out_len) {
    const uint32_t high_index = 65536;
    const char *page_tree_obj = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    size_t page_tree_len = strlen(page_tree_obj);
    size_t obj_stream_cap = 0;
    char *obj_stream = NULL;
    char *pdf = NULL;

    if ((size_t)high_index > (SIZE_MAX - page_tree_len - 64) / 4) {
        return NULL;
    }
    obj_stream_cap = (size_t)high_index * 4 + 16 + page_tree_len;
    obj_stream = (char *)malloc(obj_stream_cap);
    if (!obj_stream) return NULL;

    size_t obj_pos = 0;
    for (uint32_t i = 0; i < high_index; i++) {
        if (obj_pos + 4 > obj_stream_cap) goto fail;
        memcpy(obj_stream + obj_pos, "9 0 ", 4);
        obj_pos += 4;
    }

    if (!appendf(obj_stream, obj_stream_cap, &obj_pos, "2 0 ")) goto fail;
    size_t first = obj_pos;
    if (!appendf(obj_stream, obj_stream_cap, &obj_pos, "%s", page_tree_obj)) goto fail;

    size_t pdf_cap = obj_pos + 4096;
    pdf = (char *)malloc(pdf_cap);
    if (!pdf) goto fail;

    size_t pos = 0;
    if (!appendf(pdf, pdf_cap, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 321 654] >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N %u /First %zu /Length %zu >>\n"
                 "stream\n",
                 high_index + 1, first, obj_pos)) goto fail;
    if (pos + obj_pos + 128 > pdf_cap) goto fail;
    memcpy(pdf + pos, obj_stream, obj_pos);
    pos += obj_pos;
    if (!appendf(pdf, pdf_cap, &pos, "\nendstream\nendobj\n")) goto fail;
    free(obj_stream);
    obj_stream = NULL;

    size_t xref_obj = pos;
    uint8_t entries[6][9];
    xref_entry9(entries[0], 0, 0, 65535);
    xref_entry9(entries[1], 1, (uint32_t)obj1, 0);
    xref_objstm_entry9(entries[2], 4, high_index);
    xref_entry9(entries[3], 1, (uint32_t)obj3, 0);
    xref_entry9(entries[4], 1, (uint32_t)obj4, 0);
    xref_entry9(entries[5], 1, (uint32_t)xref_obj, 0);

    if (!appendf(pdf, pdf_cap, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 4 4] /Index [0 6] /Size 6 /Root 1 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > pdf_cap) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, pdf_cap, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(obj_stream);
    free(pdf);
    return NULL;
}

static char *make_object_stream_lzw_pdf(size_t *out_len) {
    char obj_stream[1024];
    size_t obj_pos = 0;
    uint8_t *encoded = NULL;

    const char *obj1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>";
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "1 0 2 %zu 3 %zu ",
                 strlen(obj1), strlen(obj1) + strlen(obj2))) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s%s", obj1, obj2, obj3)) {
        return NULL;
    }

    size_t encoded_len = 0;
    encoded = lzw_encode((const uint8_t *)obj_stream, obj_pos, &encoded_len);
    if (!encoded) return NULL;

    char *pdf = (char *)malloc(4096);
    if (!pdf) {
        free(encoded);
        return NULL;
    }

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 3 /First %zu /Length %zu /Filter /LZWDecode >>\n"
                 "stream\n",
                 first, encoded_len)) goto fail;
    if (pos + encoded_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    free(encoded);
    encoded = NULL;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

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
    free(pdf);
    return NULL;
}

static char *make_hybrid_reference_pdf_with_xrefstm_adjust(size_t *out_len,
                                                           int xrefstm_adjust) {
    char obj_stream[1024];
    size_t obj_pos = 0;

    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 250 350] >>";
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "2 0 3 %zu ", strlen(obj2))) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s", obj2, obj3)) {
        return NULL;
    }

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 2 /First %zu /Length %zu >>\n"
                 "stream\n",
                 first, obj_pos)) goto fail;
    if (pos + obj_pos + 128 > 4096) goto fail;
    memcpy(pdf + pos, obj_stream, obj_pos);
    pos += obj_pos;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref_stream = pos;
    uint8_t compressed_entries[2][4];
    xref_objstm_entry4(compressed_entries[0], 4, 0);
    xref_objstm_entry4(compressed_entries[1], 4, 1);
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [2 2] /Size 6 >>\n"
                 "stream\n",
                 sizeof(compressed_entries))) goto fail;
    if (pos + sizeof(compressed_entries) + 128 > 4096) goto fail;
    memcpy(pdf + pos, compressed_entries, sizeof(compressed_entries));
    pos += sizeof(compressed_entries);
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xrefstm_value = xref_stream;
    if (xrefstm_adjust < 0) {
        size_t delta = (size_t)(-(xrefstm_adjust + 1)) + 1;
        if (delta > xrefstm_value) goto fail;
        xrefstm_value -= delta;
    } else {
        size_t delta = (size_t)xrefstm_adjust;
        if (delta > SIZE_MAX - xrefstm_value) goto fail;
        xrefstm_value += delta;
    }

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 2\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "4 2\n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R /XRefStm %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj4, xref_stream, xrefstm_value, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_hybrid_reference_pdf(size_t *out_len) {
    return make_hybrid_reference_pdf_with_xrefstm_adjust(out_len, 0);
}

static char *make_malformed_xref_stream_pdf(const char *dict_entries, size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t xref_obj = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n"
                 "<< /Type /XRef %s >>\n"
                 "stream\n"
                 "endstream\n"
                 "endobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 dict_entries, xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_raw_xref_stream_pdf_with_type(const char *type_entry,
                                                const char *dict_entries,
                                                const uint8_t *stream_data,
                                                size_t stream_len,
                                                size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t xref_obj = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n"
                 "<< %s/Length %zu %s >>\n"
                 "stream\n",
                 type_entry, stream_len, dict_entries)) goto fail;
    if (pos + stream_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, stream_data, stream_len);
    pos += stream_len;
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\n"
                 "endobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_raw_xref_stream_pdf(const char *dict_entries,
                                      const uint8_t *stream_data,
                                      size_t stream_len,
                                      size_t *out_len) {
    return make_raw_xref_stream_pdf_with_type("/Type /XRef ", dict_entries,
                                              stream_data, stream_len, out_len);
}

static char *make_incremental_update_pdf_with_options(size_t *out_len,
                                                      bool omit_new_root) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF\n",
                 obj1_old, obj2_old, obj3_old, xref_old)) goto fail;

    size_t obj1_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>\nendobj\n")) goto fail;

    size_t obj5_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 4 0 R /MediaBox [0 0 600 800] >>\nendobj\n")) goto fail;

    size_t xref_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "1 1\n"
                 "%010zu 00000 n \n"
                 "4 2\n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 %s/Prev %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1_new, obj4_new, obj5_new,
                 omit_new_root ? "" : "/Root 1 0 R ", xref_old, xref_new)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_incremental_update_pdf(size_t *out_len) {
    return make_incremental_update_pdf_with_options(out_len, false);
}

static char *make_self_referential_prev_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 1\n"
                 "0000000000 65535 f \n"
                 "trailer\n<< /Size 1 /Prev %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_stream_length_pdf(size_t *out_len, size_t *stream_len) {
    const char *payload = "abc endstream def";
    size_t payload_len = strlen(payload);

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length 5 0 R >>\nstream\n%s\nendstream\nendobj\n",
                 payload)) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n%zu\nendobj\n",
                 payload_len)) goto fail;

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
                 "trailer\n<< /Size 6 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, xref)) goto fail;

    *out_len = pos;
    *stream_len = payload_len;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_page_tree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids 6 0 R /Count 1 /MediaBox 4 0 R /Rotate 5 0 R >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n[0 0 300 400]\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n90\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n[3 0 R]\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_page_box_numbers_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 4 0 R 5 0 R] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n300.5\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n400\nendobj\n")) goto fail;

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
                 "trailer\n<< /Size 6 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
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

static char *make_inherited_crop_box_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /CropBox [0 0 320 480] >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

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

static char *make_inherited_user_unit_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.6\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/MediaBox [0 0 100 200] /UserUnit 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n2.5\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
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

static char *make_noncanonical_rotate_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 "
                 "/Rotate -90 /MediaBox [0 0 300 400] >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /Rotate 450 >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
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

static char *make_invalid_rotate_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/Rotate 45 /MediaBox [0 0 300 400] >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

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

static char *make_indirect_version_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Version 4 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos, "4 0 obj\n/1.6\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
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

TEST(test_pdf_version_catalog_indirect_ref_resolved) {
    size_t len = 0;
    char *pdf = make_indirect_version_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // An indirect /Version must be resolved, not silently ignored in favor
    // of the header version.
    ASSERT_EQ_STR(tspdf_reader_pdf_version(doc), "1.6");
    // Resolution is cached; a second call returns the same answer.
    ASSERT_EQ_STR(tspdf_reader_pdf_version(doc), "1.6");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_parse_classic) {
    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 4);
    ASSERT(xref.entries[0].in_use == false); // obj 0 is free
    ASSERT(xref.entries[1].in_use == true);
    ASSERT(xref.entries[1].offset == 9);
    ASSERT(xref.trailer != NULL);
    TspdfObj *root = tspdf_dict_get(xref.trailer, "Root");
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->type, TSPDF_OBJ_REF);
    ASSERT(root->ref.num == 1);
    tspdf_arena_destroy(&a);
}

TEST(test_document_open_classic_xref_horizontal_whitespace) {
    size_t len = 0;
    char *pdf = make_classic_xref_horizontal_whitespace_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_resolve) {
    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    TspdfObj *obj1 = tspdf_xref_resolve(&xref, &p, 1, cache, NULL);
    ASSERT(obj1 != NULL);
    ASSERT_EQ_INT(obj1->type, TSPDF_OBJ_DICT);
    TspdfObj *type_val = tspdf_dict_get(obj1, "Type");
    ASSERT(type_val != NULL);
    ASSERT_EQ_STR((const char *)type_val->string.data, "Catalog");
    // Verify caching: same pointer returned
    TspdfObj *obj1b = tspdf_xref_resolve(&xref, &p, 1, cache, NULL);
    ASSERT(obj1 == obj1b);
    tspdf_arena_destroy(&a);
}

TEST(test_classic_xref_reject_subsection_range_overflow) {
    char header[64];
    snprintf(header, sizeof(header), "%zu 2", (size_t)SIZE_MAX - 1);

    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf(header, "", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_generation_overflow) {
    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf("0 1", "0000000000 65536 n \n", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_unknown_entry_type) {
    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf("0 1", "0000000000 00000 z \n", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_reject_wrong_xref_object_number) {
    size_t len = 0;
    char *pdf = make_wrong_xref_identity_pdf(&len, "9 0 obj", "00000", "0");
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_wrong_xref_generation) {
    size_t len = 0;
    char *pdf = make_wrong_xref_identity_pdf(&len, "1 0 obj", "00001", "1");
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_open_xref_stream_with_png_predictor) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_without_type) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, true, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_dp_abbreviation) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, false, false, true);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_stream_reject_explicit_wrong_type) {
    const uint8_t row[4] = {0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf_with_type("/Type /NotXRef ",
                                                  "/W [1 2 1] /Size 1",
                                                  row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_implausible_entry_count) {
    // A 1KB file cannot define 48 million objects; the declared subsection
    // count must be rejected before the entry table is allocated (no OOM).
    // The object-scan fallback then finds the catalog but there is no page
    // tree, so the open still fails cleanly (doc == NULL) rather than crashing
    // or allocating gigabytes.
    static const char pdf[] =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "xref\n"
        "0 48000000\n"
        "0000000000 65535 f \n"
        "trailer\n<< /Size 48000000 /Root 1 0 R >>\n"
        "startxref\n58\n%%EOF";

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, sizeof(pdf) - 1, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_xref_stream_reject_implausible_size) {
    const uint8_t row[4] = {1, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 1] /Size 48000000",
                                         row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_filter_array) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, true, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_filter_chain_direct_decode_params) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, true, false, false, false, true, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_ascii85_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, true, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_stream_ascii85_z_shorthand_expands_safely) {
    const uint8_t data[] = "zzz~>";
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/Filter /ASCII85Decode /W [1 2 1] /Size 3",
                                         data, sizeof(data) - 1, &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 3);
    ASSERT(xref.entries[0].seen == true);
    ASSERT(xref.entries[1].seen == true);
    ASSERT(xref.entries[2].seen == true);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_runlength_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, false, true, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_lzw_predictor) {
    size_t len = 0;
    char *pdf = make_xref_stream_lzw_predictor_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_xref_stream_negative_w) {
    size_t len = 0;
    char *pdf = make_malformed_xref_stream_pdf("/Length 0 /W [1 -1 2] /Size 1", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_xref_stream_odd_index) {
    size_t len = 0;
    char *pdf = make_malformed_xref_stream_pdf("/Length 0 /W [1 2 1] /Size 1 /Index [0]", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_xref_stream_reject_oversized_field_width) {
    const uint8_t row[11] = {0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 9 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_unknown_entry_type) {
    const uint8_t row[4] = {3, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_generation_overflow) {
    const uint8_t row[6] = {1, 0, 0, 1, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 3] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_objstm_number_overflow) {
    const uint8_t row[7] = {2, 1, 0, 0, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 5 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_accepts_large_objstm_index) {
    uint8_t row[9];
    xref_objstm_entry9(row, 1, 65536);
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 4 4] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 1);
    ASSERT(xref.entries[0].compressed);
    ASSERT_EQ_INT(xref.entries[0].stream_obj, 1);
    ASSERT_EQ_SIZE(xref.entries[0].stream_idx, 65536);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_reject_self_referential_object_stream_entry) {
    size_t len = 0;
    char *pdf = make_recursive_compressed_xref_pdf(false, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    free(pdf);
}

TEST(test_document_reject_cyclic_object_stream_entries) {
    size_t len = 0;
    char *pdf = make_recursive_compressed_xref_pdf(true, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    free(pdf);
}

TEST(test_document_open_incremental_update_prev_chain) {
    size_t len = 0;
    char *pdf = make_incremental_update_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *old_page = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *new_page = tspdf_reader_get_page(doc, 1);
    ASSERT(old_page != NULL);
    ASSERT(new_page != NULL);
    ASSERT(old_page->media_box[2] == 300.0);
    ASSERT(old_page->media_box[3] == 400.0);
    ASSERT(new_page->media_box[2] == 600.0);
    ASSERT(new_page->media_box[3] == 800.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_incremental_update_inherits_root_from_prev) {
    size_t len = 0;
    char *pdf = make_incremental_update_pdf_with_options(&len, true);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *old_page = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *new_page = tspdf_reader_get_page(doc, 1);
    ASSERT(old_page != NULL);
    ASSERT(new_page != NULL);
    ASSERT(old_page->media_box[2] == 300.0);
    ASSERT(old_page->media_box[3] == 400.0);
    ASSERT(new_page->media_box[2] == 600.0);
    ASSERT(new_page->media_box[3] == 800.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_reject_self_referential_prev_chain) {
    size_t len = 0;
    char *pdf = make_self_referential_prev_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_hybrid_reference_xrefstm) {
    size_t len = 0;
    char *pdf = make_hybrid_reference_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 250.0);
    ASSERT(page->media_box[3] == 350.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_hybrid_reference_xrefstm_near_offset_repaired) {
    size_t len = 0;
    char *pdf = make_hybrid_reference_pdf_with_xrefstm_adjust(&len, 5);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 250.0);
    ASSERT(page->media_box[3] == 350.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_indirect_stream_length) {
    size_t len = 0;
    size_t stream_len = 0;
    char *pdf = make_indirect_stream_length_pdf(&len, &stream_len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    ASSERT(cache != NULL);

    TspdfObj *stream = tspdf_xref_resolve(&xref, &p, 4, cache, NULL);
    ASSERT(stream != NULL);
    ASSERT_EQ_INT(stream->type, TSPDF_OBJ_STREAM);
    ASSERT_EQ_SIZE(stream->stream.raw_len, stream_len);
    ASSERT(memcmp(p.data + stream->stream.raw_offset, "abc endstream def", stream_len) == 0);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_resolve_rejects_objstm_header_spill) {
    size_t len = 0;
    char *pdf = make_object_stream_zero_first_header_spill_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    ASSERT(cache != NULL);

    TspdfObj *obj = tspdf_xref_resolve(&xref, &p, 2, cache, NULL);
    ASSERT(obj == NULL);

    // The failed resolve leaves the decoded ObjStm buffer on the cached
    // stream object; the document path frees it in tspdf_reader_destroy,
    // but this direct-resolve harness must free it itself.
    for (size_t i = 0; i < xref.count; i++) {
        if (cache[i] && cache[i]->type == TSPDF_OBJ_STREAM) {
            free(cache[i]->stream.data);
        }
    }

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_object_stream_page_tree) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_large_index) {
    size_t len = 0;
    char *pdf = make_object_stream_high_index_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 321.0);
    ASSERT(page->media_box[3] == 654.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, true, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_ascii85_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, false, true, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_runlength_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, false, false, true);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_lzw_filter) {
    size_t len = 0;
    char *pdf = make_object_stream_lzw_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_objstm_negative_first) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, "-1", NULL, NULL, NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_negative_n) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, "-1", NULL, NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_bad_offset) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, "9999", NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_wrong_object_number) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, "9", false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_open_mini) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    // MediaBox [0 0 612 792]
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);
    ASSERT_EQ_INT(page->rotate, 0);
    tspdf_reader_destroy(doc);
}

TEST(test_document_open_direct_trailer_root_catalog) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("<< /Type /Catalog /Pages 1 0 R >>", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 450.0);
    ASSERT(page->media_box[3] == 650.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_duplicate_trailer_root_uses_last) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("9 0 R /Root << /Type /Catalog /Pages 1 0 R >>", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 450.0);
    ASSERT(page->media_box[3] == 650.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_non_dictionary_trailer_root) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("/Catalog", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_open_invalid) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)"not a pdf", 9, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_document_open_header_with_prefix) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf("%!PS-Adobe-3.0\n", 0, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_header_beyond_1024_byte_prefix) {
    // 2048 bytes of filler push the "%PDF-" header past the fast-path window.
    size_t len = 0;
    char *pdf = make_large_prefixed_pdf(2048, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_prefixed_header_relative_xref_offsets_repaired) {
    size_t len = 0;
    char *pdf = make_prefixed_header_relative_xref_pdf("%!PS-Adobe-3.0\n", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_long_trailing_padding) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf(NULL, 5000, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_startxref_before_table_repaired) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf_with_startxref_adjust(NULL, 0, -3, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_startxref_inside_table_repaired) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf_with_startxref_adjust(NULL, 0, 2, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_bad_appended_startxref_uses_earlier_marker) {
    size_t len = 0;
    char *pdf = make_pdf_with_bad_appended_startxref(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_missing_startxref_classic_xref_recovered) {
    size_t len = 0;
    char *pdf = make_missing_startxref_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_no_trailer_object_scan_recovered) {
    // No trailer / no startxref: only objects and xref rows. Reconstruct by
    // scanning "N G obj" markers and locating /Type /Catalog.
    size_t len = 0;
    char *pdf = make_no_trailer_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_scan_only_recovered) {
    // No xref, no trailer, no startxref, and the catalog is not object 1.
    size_t len = 0;
    char *pdf = make_object_scan_only_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 200.0);
    ASSERT(page->media_box[3] == 300.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_short_header_only) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)"%PDF-", 5, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_document_page_inherit_rotate) {
    // PDF where /Rotate is on the /Pages node, inherited by child
    static const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /Rotate 90 /MediaBox [0 0 612 792] >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n"
        "xref\n"
        "0 4\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000150 00000 n \n"
        "trailer\n<< /TspdfSize 4 /Root 1 0 R >>\n"
        "startxref\n197\n%%EOF";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 90);
    ASSERT(page->media_box[2] == 612.0);
    tspdf_reader_destroy(doc);
}

TEST(test_document_page_tree_normalizes_rotate) {
    size_t len = 0;
    char *pdf = make_noncanonical_rotate_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *inherited = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *direct = tspdf_reader_get_page(doc, 1);
    ASSERT(inherited != NULL);
    ASSERT(direct != NULL);
    ASSERT_EQ_INT(inherited->rotate, 270);
    ASSERT_EQ_INT(direct->rotate, 90);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_ignores_invalid_rotate) {
    size_t len = 0;
    char *pdf = make_invalid_rotate_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_indirect_attributes) {
    size_t len = 0;
    char *pdf = make_indirect_page_tree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);
    ASSERT_EQ_INT(page->rotate, 90);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_box_indirect_number_entries) {
    size_t len = 0;
    char *pdf = make_indirect_page_box_numbers_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 300.5);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_missing_type_inferred) {
    size_t len = 0;
    char *pdf = make_missing_page_tree_type_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 320.0);
    ASSERT(page->media_box[3] == 420.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_inherited_crop_box_fallback) {
    size_t len = 0;
    char *pdf = make_inherited_crop_box_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 320.0);
    ASSERT(page->media_box[3] == 480.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_inherited_user_unit) {
    size_t len = 0;
    char *pdf = make_inherited_user_unit_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 100.0);
    ASSERT(page->media_box[3] == 200.0);
    ASSERT(page->user_unit == 2.5);

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

    TspdfReaderPage *roundtrip_page = tspdf_reader_get_page(reopened, 0);
    ASSERT(roundtrip_page != NULL);
    ASSERT(roundtrip_page->media_box[2] == 100.0);
    ASSERT(roundtrip_page->media_box[3] == 200.0);
    ASSERT(roundtrip_page->user_unit == 2.5);

    TspdfObj *unit = tspdf_dict_get(roundtrip_page->page_dict, "UserUnit");
    ASSERT(unit != NULL);
    if (unit->type == TSPDF_OBJ_REF) {
        TspdfParser parser;
        tspdf_parser_init(&parser, reopened->data, reopened->data_len, &reopened->arena);
        unit = tspdf_xref_resolve(&reopened->xref, &parser, unit->ref.num, reopened->obj_cache, NULL);
        ASSERT(unit != NULL);
    }
    ASSERT_EQ_INT(unit->type, TSPDF_OBJ_REAL);
    ASSERT(unit->real == 2.5);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
}

void run_xref_tests(void) {
    printf("\n  Xref:\n");
    RUN(test_xref_parse_classic);
    RUN(test_pdf_version_catalog_indirect_ref_resolved);
    RUN(test_document_open_classic_xref_horizontal_whitespace);
    RUN(test_xref_resolve);
    RUN(test_classic_xref_reject_subsection_range_overflow);
    RUN(test_classic_xref_reject_generation_overflow);
    RUN(test_classic_xref_reject_unknown_entry_type);
    RUN(test_document_reject_wrong_xref_object_number);
    RUN(test_document_reject_wrong_xref_generation);
    RUN(test_document_open_xref_stream_with_png_predictor);
    RUN(test_document_open_xref_stream_without_type);
    RUN(test_document_open_xref_stream_with_dp_abbreviation);
    RUN(test_xref_stream_reject_explicit_wrong_type);
    RUN(test_document_open_xref_stream_with_filter_array);
    RUN(test_document_open_xref_stream_with_filter_chain);
    RUN(test_document_open_xref_stream_filter_chain_direct_decode_params);
    RUN(test_document_open_xref_stream_with_ascii85_filter_chain);
    RUN(test_xref_stream_ascii85_z_shorthand_expands_safely);
    RUN(test_document_open_xref_stream_with_runlength_filter_chain);
    RUN(test_document_open_xref_stream_with_lzw_predictor);
    RUN(test_document_reject_xref_stream_negative_w);
    RUN(test_document_reject_xref_stream_odd_index);
    RUN(test_xref_stream_reject_oversized_field_width);
    RUN(test_classic_xref_reject_implausible_entry_count);
    RUN(test_xref_stream_reject_implausible_size);
    RUN(test_xref_stream_reject_unknown_entry_type);
    RUN(test_xref_stream_reject_generation_overflow);
    RUN(test_xref_stream_reject_objstm_number_overflow);
    RUN(test_xref_stream_accepts_large_objstm_index);
    RUN(test_document_reject_self_referential_object_stream_entry);
    RUN(test_document_reject_cyclic_object_stream_entries);
    RUN(test_document_open_incremental_update_prev_chain);
    RUN(test_document_open_incremental_update_inherits_root_from_prev);
    RUN(test_xref_reject_self_referential_prev_chain);
    RUN(test_document_open_hybrid_reference_xrefstm);
    RUN(test_document_open_hybrid_reference_xrefstm_near_offset_repaired);
    RUN(test_xref_indirect_stream_length);
    RUN(test_xref_resolve_rejects_objstm_header_spill);
    RUN(test_document_open_object_stream_page_tree);
    RUN(test_document_open_object_stream_large_index);
    RUN(test_document_open_object_stream_filter_chain);
    RUN(test_document_open_object_stream_ascii85_filter_chain);
    RUN(test_document_open_object_stream_runlength_filter_chain);
    RUN(test_document_open_object_stream_lzw_filter);
    RUN(test_document_reject_objstm_negative_first);
    RUN(test_document_reject_objstm_negative_n);
    RUN(test_document_reject_objstm_bad_offset);
    RUN(test_document_reject_objstm_wrong_object_number);
    printf("\n  Document open:\n");
    RUN(test_document_open_mini);
    RUN(test_document_open_direct_trailer_root_catalog);
    RUN(test_document_open_duplicate_trailer_root_uses_last);
    RUN(test_document_reject_non_dictionary_trailer_root);
    RUN(test_document_open_invalid);
    RUN(test_document_open_header_with_prefix);
    RUN(test_document_open_header_beyond_1024_byte_prefix);
    RUN(test_document_open_prefixed_header_relative_xref_offsets_repaired);
    RUN(test_document_open_long_trailing_padding);
    RUN(test_document_open_startxref_before_table_repaired);
    RUN(test_document_open_startxref_inside_table_repaired);
    RUN(test_document_open_bad_appended_startxref_uses_earlier_marker);
    RUN(test_document_open_missing_startxref_classic_xref_recovered);
    RUN(test_document_open_no_trailer_object_scan_recovered);
    RUN(test_document_open_object_scan_only_recovered);
    RUN(test_document_open_short_header_only);
    RUN(test_document_page_inherit_rotate);
    RUN(test_document_page_tree_normalizes_rotate);
    RUN(test_document_page_tree_ignores_invalid_rotate);
    RUN(test_document_page_tree_indirect_attributes);
    RUN(test_document_page_box_indirect_number_entries);
    RUN(test_document_page_tree_missing_type_inferred);
    RUN(test_document_page_tree_inherited_crop_box_fallback);
    RUN(test_document_page_tree_inherited_user_unit);
}
