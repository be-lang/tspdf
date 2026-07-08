#include "tspr_internal.h"
#include "../compress/deflate.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>

// --- Helpers ---

// Find the Nth startxref from the end (0 = last, 1 = second-to-last, etc.).
// Some real PDFs contain long trailing signatures, comments, or transfer padding
// after %%EOF, so scan the whole buffer instead of only the final few KiB.
static bool find_startxref_nth(const uint8_t *data, size_t len, size_t *out_offset, int skip) {
    const char *needle = "startxref";
    size_t needle_len = 9;
    int found = 0;

    if (!data || !out_offset || len < needle_len) {
        return false;
    }

    for (size_t cursor = len - needle_len + 1; cursor > 0; cursor--) {
        size_t i = cursor - 1;
        if (memcmp(data + i, needle, needle_len) == 0) {
            if (found < skip) { found++; continue; }
            // Parse the offset that follows
            size_t pos = i + needle_len;
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                                  data[pos] == '\r' || data[pos] == '\n')) {
                pos++;
            }
            size_t num_start = pos;
            while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
                pos++;
            }
            if (pos == num_start) { if (i == 0) break; found++; continue; }

            char buf[32];
            size_t nlen = pos - num_start;
            if (nlen >= sizeof(buf)) { found++; continue; }
            memcpy(buf, data + num_start, nlen);
            buf[nlen] = '\0';
            errno = 0;
            char *end = NULL;
            unsigned long long value = strtoull(buf, &end, 10);
            if (errno == ERANGE || !end || *end != '\0') {
                found++;
                continue;
            }
#if SIZE_MAX < ULLONG_MAX
            if (value > (unsigned long long)SIZE_MAX) {
                found++;
                continue;
            }
#endif
            *out_offset = (size_t)value;
            return true;
        }
    }
    return false;
}

static bool checked_add_size(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

// Cap xref table growth by what the input could plausibly define: even the
// tersest indirect object plus its xref row takes well over four bytes, so a
// file of L bytes cannot introduce more than ~L/4 objects. Without this cap a
// malformed subsection header or /Size forces multi-GB allocations from a
// tiny input (found by fuzzing).
static size_t max_plausible_objects(size_t file_len) {
    return file_len / 4 + 16;
}

static bool ensure_xref_capacity(TspdfReaderXref *xref, size_t needed, TspdfArena *arena) {
    if (needed <= xref->count) return true;
    size_t alloc_size;
    if (!checked_mul_size(sizeof(TspdfReaderXrefEntry), needed, &alloc_size)) {
        return false;
    }
    TspdfReaderXrefEntry *new_entries =
        (TspdfReaderXrefEntry *)tspdf_arena_alloc_zero(arena, alloc_size);
    if (!new_entries) return false;
    if (xref->entries && xref->count > 0) {
        memcpy(new_entries, xref->entries, sizeof(TspdfReaderXrefEntry) * xref->count);
    }
    xref->entries = new_entries;
    xref->count = needed;
    return true;
}

static bool uint64_fits_size_t(uint64_t value) {
#if SIZE_MAX < UINT64_MAX
    return value <= (uint64_t)SIZE_MAX;
#else
    (void)value;
    return true;
#endif
}

static bool parse_decimal_u64(const char *buf, uint64_t *out) {
    if (!buf || !out || buf[0] == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(buf, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') {
        return false;
    }

    *out = (uint64_t)value;
    return true;
}

static bool parse_decimal_size(const char *buf, size_t *out) {
    uint64_t value = 0;
    if (!parse_decimal_u64(buf, &value) || !uint64_fits_size_t(value)) {
        return false;
    }

    *out = (size_t)value;
    return true;
}

static bool is_xref_inline_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\0' || c == '\f';
}

static bool obj_name_equals(TspdfObj *obj, const char *name) {
    return obj && obj->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)obj->string.data, name) == 0;
}

static TspdfObj *decode_params_at(TspdfObj *stream_dict, size_t filter_index) {
    TspdfObj *params = tspdf_dict_get(stream_dict, "DecodeParms");
    if (!params) {
        params = tspdf_dict_get(stream_dict, "DP");
    }
    if (!params || params->type == TSPDF_OBJ_NULL) {
        return NULL;
    }

    if (params->type == TSPDF_OBJ_DICT) {
        (void)filter_index;
        return params;
    }

    if (params->type == TSPDF_OBJ_ARRAY && filter_index < params->array.count) {
        TspdfObj *entry = &params->array.items[filter_index];
        if (entry->type == TSPDF_OBJ_DICT) {
            return entry;
        }
    }

    return NULL;
}

static bool decode_param_int(TspdfObj *params, const char *key, int default_value, int *out) {
    *out = default_value;
    if (!params) {
        return true;
    }

    TspdfObj *obj = tspdf_dict_get(params, key);
    if (!obj) {
        return true;
    }
    if (obj->type != TSPDF_OBJ_INT || obj->integer < 0 || obj->integer > INT_MAX) {
        return false;
    }

    *out = (int)obj->integer;
    return true;
}

static uint8_t paeth_predictor(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static void decode_png_predictor_row(uint8_t *dst, const uint8_t *src,
                                     const uint8_t *prev, size_t row_len,
                                     size_t bytes_per_pixel, uint8_t filter) {
    for (size_t i = 0; i < row_len; i++) {
        uint8_t left = (i >= bytes_per_pixel) ? dst[i - bytes_per_pixel] : 0;
        uint8_t up = prev ? prev[i] : 0;
        uint8_t up_left = (prev && i >= bytes_per_pixel) ? prev[i - bytes_per_pixel] : 0;
        uint8_t predicted = 0;

        switch (filter) {
            case 0: predicted = 0; break;
            case 1: predicted = left; break;
            case 2: predicted = up; break;
            case 3: predicted = (uint8_t)(((int)left + (int)up) / 2); break;
            case 4: predicted = paeth_predictor(left, up, up_left); break;
            default: predicted = 0; break;
        }

        dst[i] = (uint8_t)(src[i] + predicted);
    }
}

static TspdfError apply_flate_decode_params(TspdfObj *params, uint8_t **data, size_t *len) {
    int predictor = 1;
    int colors = 1;
    int bits_per_component = 8;
    int columns = 1;
    if (!decode_param_int(params, "Predictor", 1, &predictor) ||
        !decode_param_int(params, "Colors", 1, &colors) ||
        !decode_param_int(params, "BitsPerComponent", 8, &bits_per_component) ||
        !decode_param_int(params, "Columns", 1, &columns)) {
        return TSPDF_ERR_XREF;
    }

    if (predictor <= 1) {
        return TSPDF_OK;
    }
    if (!data || !*data || !len || colors <= 0 || columns <= 0 || bits_per_component <= 0) {
        return TSPDF_ERR_XREF;
    }

    // XRef and object streams generated by pdfTeX use 8-bit PNG predictors.
    // Other bit depths need bit-level sample handling, so reject them clearly.
    if (bits_per_component != 8) {
        return TSPDF_ERR_UNSUPPORTED;
    }

    if ((size_t)columns > SIZE_MAX / (size_t)colors) {
        return TSPDF_ERR_XREF;
    }
    size_t samples = (size_t)columns * (size_t)colors;
    if (samples > (SIZE_MAX - 7) / (size_t)bits_per_component) {
        return TSPDF_ERR_XREF;
    }
    size_t row_len = (samples * (size_t)bits_per_component + 7) / 8;
    size_t bytes_per_pixel = ((size_t)colors * (size_t)bits_per_component + 7) / 8;
    if (row_len == 0 || bytes_per_pixel == 0) {
        return TSPDF_ERR_XREF;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;

    if (predictor == 2) {
        if (*len % row_len != 0) {
            return TSPDF_ERR_XREF;
        }
        out_len = *len;
        out = (uint8_t *)malloc(out_len);
        if (!out && out_len > 0) {
            return TSPDF_ERR_ALLOC;
        }

        size_t rows = *len / row_len;
        for (size_t r = 0; r < rows; r++) {
            const uint8_t *src = *data + r * row_len;
            uint8_t *dst = out + r * row_len;
            decode_png_predictor_row(dst, src, NULL, row_len, bytes_per_pixel, 1);
        }
    } else if (predictor >= 10 && predictor <= 15) {
        size_t tagged_row_len = row_len + 1;

        if (*len % tagged_row_len == 0) {
            size_t rows = *len / tagged_row_len;
            out_len = rows * row_len;
            out = (uint8_t *)malloc(out_len);
            if (!out && out_len > 0) {
                return TSPDF_ERR_ALLOC;
            }

            const uint8_t *prev = NULL;
            for (size_t r = 0; r < rows; r++) {
                const uint8_t *src = *data + r * tagged_row_len;
                uint8_t filter = src[0];
                if (filter > 4) {
                    free(out);
                    return TSPDF_ERR_XREF;
                }

                uint8_t *dst = out + r * row_len;
                decode_png_predictor_row(dst, src + 1, prev, row_len, bytes_per_pixel, filter);
                prev = dst;
            }
        } else if (predictor <= 14 && *len % row_len == 0) {
            size_t rows = *len / row_len;
            out_len = *len;
            out = (uint8_t *)malloc(out_len);
            if (!out && out_len > 0) {
                return TSPDF_ERR_ALLOC;
            }

            const uint8_t *prev = NULL;
            uint8_t filter = (uint8_t)(predictor - 10);
            for (size_t r = 0; r < rows; r++) {
                const uint8_t *src = *data + r * row_len;
                uint8_t *dst = out + r * row_len;
                decode_png_predictor_row(dst, src, prev, row_len, bytes_per_pixel, filter);
                prev = dst;
            }
        } else {
            return TSPDF_ERR_XREF;
        }
    } else {
        return TSPDF_ERR_UNSUPPORTED;
    }

    free(*data);
    *data = out;
    *len = out_len;
    return TSPDF_OK;
}

static TspdfError decode_ascii_hex(const uint8_t *data, size_t len,
                                   uint8_t **out, size_t *out_len) {
    size_t cap = len / 2 + 2;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return TSPDF_ERR_ALLOC;
    }

    size_t pos = 0;
    int high = -1;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '>') {
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f') {
            continue;
        }

        int value;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else {
            free(buf);
            return TSPDF_ERR_XREF;
        }

        if (high < 0) {
            high = value;
        } else {
            if (pos >= cap) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            buf[pos++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }

    if (high >= 0) {
        if (pos >= cap) {
            free(buf);
            return TSPDF_ERR_XREF;
        }
        buf[pos++] = (uint8_t)(high << 4);
    }

    *out = buf;
    *out_len = pos;
    return TSPDF_OK;
}

static bool ensure_decode_capacity(uint8_t **buf, size_t *cap, size_t len, size_t extra) {
    if (extra > SIZE_MAX - len) {
        return false;
    }
    size_t needed = len + extra;
    if (needed <= *cap) {
        return true;
    }

    size_t new_cap = *cap ? *cap : 256;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = (uint8_t *)realloc(*buf, new_cap);
    if (!new_buf) {
        return false;
    }
    *buf = new_buf;
    *cap = new_cap;
    return true;
}

static TspdfError decode_ascii85(const uint8_t *data, size_t len,
                                 uint8_t **out, size_t *out_len) {
    if (len > SIZE_MAX - 4) {
        return TSPDF_ERR_XREF;
    }

    size_t cap = len + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return TSPDF_ERR_ALLOC;
    }

    size_t pos = 0;
    uint32_t digits[5];
    size_t count = 0;
    bool eod = false;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f') {
            continue;
        }

        if (c == '~') {
            size_t j = i + 1;
            while (j < len && (data[j] == ' ' || data[j] == '\t' ||
                               data[j] == '\r' || data[j] == '\n' ||
                               data[j] == '\0' || data[j] == '\f')) {
                j++;
            }
            if (j >= len || data[j] != '>') {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            eod = true;
            break;
        }

        if (c == 'z') {
            if (count != 0) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            if (!ensure_decode_capacity(&buf, &cap, pos, 4)) {
                free(buf);
                return TSPDF_ERR_ALLOC;
            }
            buf[pos++] = 0;
            buf[pos++] = 0;
            buf[pos++] = 0;
            buf[pos++] = 0;
            continue;
        }

        if (c < '!' || c > 'u') {
            free(buf);
            return TSPDF_ERR_XREF;
        }

        digits[count++] = (uint32_t)(c - '!');
        if (count == 5) {
            uint64_t value = 0;
            for (size_t d = 0; d < 5; d++) {
                value = value * 85 + digits[d];
            }
            if (value > UINT32_MAX) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            if (!ensure_decode_capacity(&buf, &cap, pos, 4)) {
                free(buf);
                return TSPDF_ERR_ALLOC;
            }
            buf[pos++] = (uint8_t)(value >> 24);
            buf[pos++] = (uint8_t)(value >> 16);
            buf[pos++] = (uint8_t)(value >> 8);
            buf[pos++] = (uint8_t)value;
            count = 0;
        }
    }

    (void)eod;
    if (count == 1) {
        free(buf);
        return TSPDF_ERR_XREF;
    }
    if (count > 1) {
        for (size_t i = count; i < 5; i++) {
            digits[i] = 84;
        }

        uint64_t value = 0;
        for (size_t d = 0; d < 5; d++) {
            value = value * 85 + digits[d];
        }
        if (value > UINT32_MAX) {
            free(buf);
            return TSPDF_ERR_XREF;
        }

        if (!ensure_decode_capacity(&buf, &cap, pos, count - 1)) {
            free(buf);
            return TSPDF_ERR_ALLOC;
        }
        for (size_t i = 0; i < count - 1; i++) {
            buf[pos++] = (uint8_t)(value >> (24 - i * 8));
        }
    }

    *out = buf;
    *out_len = pos;
    return TSPDF_OK;
}

static TspdfError decode_run_length(const uint8_t *data, size_t len,
                                    uint8_t **out, size_t *out_len) {
    size_t cap = len ? len : 1;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return TSPDF_ERR_ALLOC;
    }

    size_t pos = 0;
    for (size_t i = 0; i < len; ) {
        uint8_t control = data[i++];
        if (control == 128) {
            break;
        }

        if (control <= 127) {
            size_t count = (size_t)control + 1;
            if (count > len - i) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            if (!ensure_decode_capacity(&buf, &cap, pos, count)) {
                free(buf);
                return TSPDF_ERR_ALLOC;
            }
            memcpy(buf + pos, data + i, count);
            pos += count;
            i += count;
        } else {
            size_t count = 257u - (size_t)control;
            if (i >= len) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            if (!ensure_decode_capacity(&buf, &cap, pos, count)) {
                free(buf);
                return TSPDF_ERR_ALLOC;
            }
            memset(buf + pos, data[i++], count);
            pos += count;
        }
    }

    *out = buf;
    *out_len = pos;
    return TSPDF_OK;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bit_pos;
} LzwBitReader;

static bool lzw_read_code(LzwBitReader *reader, int bits, uint16_t *out_code) {
    if (!reader || !out_code || bits <= 0 || bits > 12 ||
        reader->len > SIZE_MAX / 8) {
        return false;
    }

    size_t total_bits = reader->len * 8;
    if (reader->bit_pos > total_bits ||
        (size_t)bits > total_bits - reader->bit_pos) {
        return false;
    }

    uint16_t value = 0;
    for (int i = 0; i < bits; i++) {
        size_t bit = reader->bit_pos++;
        uint8_t byte = reader->data[bit / 8];
        value = (uint16_t)((value << 1) | ((byte >> (7 - (bit % 8))) & 1u));
    }

    *out_code = value;
    return true;
}

static void lzw_reset_table(uint16_t *prefix, uint8_t *suffix,
                            size_t *next_code, int *code_size) {
    for (size_t i = 0; i < 256; i++) {
        prefix[i] = 0;
        suffix[i] = (uint8_t)i;
    }
    *next_code = 258;
    *code_size = 9;
}

static bool lzw_expand_code(uint16_t code, size_t next_code,
                            const uint16_t *prefix, const uint8_t *suffix,
                            uint8_t *sequence, size_t *sequence_len,
                            uint8_t *first_char) {
    uint8_t stack[4096];
    size_t stack_len = 0;
    uint16_t current = code;

    while (current >= 258) {
        if ((size_t)current >= next_code || stack_len >= sizeof(stack)) {
            return false;
        }
        stack[stack_len++] = suffix[current];
        current = prefix[current];
    }

    if (current >= 256 || stack_len >= sizeof(stack)) {
        return false;
    }

    stack[stack_len++] = (uint8_t)current;
    *first_char = (uint8_t)current;

    for (size_t i = 0; i < stack_len; i++) {
        sequence[i] = stack[stack_len - 1 - i];
    }
    *sequence_len = stack_len;
    return true;
}

static void lzw_maybe_grow_code_size(size_t next_code, int early_change,
                                     int *code_size) {
    if (*code_size < 12 &&
        next_code + (size_t)early_change >= ((size_t)1 << *code_size)) {
        (*code_size)++;
    }
}

static TspdfError decode_lzw(const uint8_t *data, size_t len,
                             TspdfObj *params,
                             uint8_t **out, size_t *out_len) {
    int early_change = 1;
    if (!decode_param_int(params, "EarlyChange", 1, &early_change) ||
        (early_change != 0 && early_change != 1)) {
        return TSPDF_ERR_XREF;
    }

    if (len > SIZE_MAX / 2) {
        return TSPDF_ERR_XREF;
    }
    size_t cap = len ? len * 2 : 1;
    if (cap < 256) cap = 256;

    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return TSPDF_ERR_ALLOC;
    }

    uint16_t prefix[4096] = {0};
    uint8_t suffix[4096] = {0};
    size_t next_code = 258;
    int code_size = 9;
    lzw_reset_table(prefix, suffix, &next_code, &code_size);

    LzwBitReader reader = {data, len, 0};
    int previous_code = -1;
    size_t pos = 0;

    while (1) {
        uint16_t code = 0;
        if (!lzw_read_code(&reader, code_size, &code)) {
            break;
        }

        if (code == 256) {
            lzw_reset_table(prefix, suffix, &next_code, &code_size);
            previous_code = -1;
            continue;
        }
        if (code == 257) {
            break;
        }

        uint8_t sequence[4096];
        size_t sequence_len = 0;
        uint8_t first_char = 0;
        if ((size_t)code < next_code) {
            if (!lzw_expand_code(code, next_code, prefix, suffix,
                                 sequence, &sequence_len, &first_char)) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
        } else if ((size_t)code == next_code && previous_code >= 0) {
            if (!lzw_expand_code((uint16_t)previous_code, next_code, prefix, suffix,
                                 sequence, &sequence_len, &first_char) ||
                sequence_len >= sizeof(sequence)) {
                free(buf);
                return TSPDF_ERR_XREF;
            }
            sequence[sequence_len++] = first_char;
        } else {
            free(buf);
            return TSPDF_ERR_XREF;
        }

        if (!ensure_decode_capacity(&buf, &cap, pos, sequence_len)) {
            free(buf);
            return TSPDF_ERR_ALLOC;
        }
        memcpy(buf + pos, sequence, sequence_len);
        pos += sequence_len;

        if (previous_code >= 0 && next_code < 4096) {
            prefix[next_code] = (uint16_t)previous_code;
            suffix[next_code] = first_char;
            next_code++;
            lzw_maybe_grow_code_size(next_code, early_change, &code_size);
        }

        previous_code = (int)code;
    }

    *out = buf;
    *out_len = pos;
    return TSPDF_OK;
}

static TspdfError decode_stream_data(TspdfObj *stream_dict,
                                     const uint8_t *raw_data, size_t raw_len,
                                     uint8_t **out_data, size_t *out_len) {
    TspdfObj *filter = tspdf_dict_get(stream_dict, "Filter");
    if (!filter || filter->type == TSPDF_OBJ_NULL) {
        uint8_t *copy = (uint8_t *)malloc(raw_len ? raw_len : 1);
        if (!copy) {
            return TSPDF_ERR_ALLOC;
        }
        if (raw_len > 0) {
            memcpy(copy, raw_data, raw_len);
        }
        *out_data = copy;
        *out_len = raw_len;
        return TSPDF_OK;
    }

    size_t filter_count = 0;
    if (filter->type == TSPDF_OBJ_NAME) {
        filter_count = 1;
    } else if (filter->type == TSPDF_OBJ_ARRAY) {
        filter_count = filter->array.count;
    } else {
        return TSPDF_ERR_UNSUPPORTED;
    }

    uint8_t *current = (uint8_t *)malloc(raw_len ? raw_len : 1);
    if (!current) {
        return TSPDF_ERR_ALLOC;
    }
    if (raw_len > 0) {
        memcpy(current, raw_data, raw_len);
    }
    size_t current_len = raw_len;

    for (size_t i = 0; i < filter_count; i++) {
        TspdfObj *filter_name = filter->type == TSPDF_OBJ_NAME ? filter : &filter->array.items[i];
        if (!filter_name || filter_name->type != TSPDF_OBJ_NAME) {
            free(current);
            return TSPDF_ERR_UNSUPPORTED;
        }

        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        if (obj_name_equals(filter_name, "ASCIIHexDecode") ||
            obj_name_equals(filter_name, "AHx")) {
            TspdfError err = decode_ascii_hex(current, current_len, &decoded, &decoded_len);
            free(current);
            if (err != TSPDF_OK) {
                return err;
            }
            current = decoded;
            current_len = decoded_len;
        } else if (obj_name_equals(filter_name, "ASCII85Decode") ||
                   obj_name_equals(filter_name, "A85")) {
            TspdfError err = decode_ascii85(current, current_len, &decoded, &decoded_len);
            free(current);
            if (err != TSPDF_OK) {
                return err;
            }
            current = decoded;
            current_len = decoded_len;
        } else if (obj_name_equals(filter_name, "RunLengthDecode") ||
                   obj_name_equals(filter_name, "RL")) {
            TspdfError err = decode_run_length(current, current_len, &decoded, &decoded_len);
            free(current);
            if (err != TSPDF_OK) {
                return err;
            }
            current = decoded;
            current_len = decoded_len;
        } else if (obj_name_equals(filter_name, "LZWDecode") ||
                   obj_name_equals(filter_name, "LZW")) {
            TspdfObj *params = decode_params_at(stream_dict, i);
            TspdfError err = decode_lzw(current, current_len, params, &decoded, &decoded_len);
            free(current);
            if (err != TSPDF_OK) {
                return err;
            }
            current = decoded;
            current_len = decoded_len;

            err = apply_flate_decode_params(params, &current, &current_len);
            if (err != TSPDF_OK) {
                free(current);
                return err;
            }
        } else if (obj_name_equals(filter_name, "FlateDecode") ||
                   obj_name_equals(filter_name, "Fl")) {
            decoded = deflate_decompress(current, current_len, &decoded_len);
            free(current);
            if (!decoded) {
                return TSPDF_ERR_XREF;
            }
            current = decoded;
            current_len = decoded_len;

            TspdfError err = apply_flate_decode_params(decode_params_at(stream_dict, i),
                                                       &current, &current_len);
            if (err != TSPDF_OK) {
                free(current);
                return err;
            }
        } else {
            free(current);
            return TSPDF_ERR_UNSUPPORTED;
        }
    }

    *out_data = current;
    *out_len = current_len;
    return TSPDF_OK;
}

// Parse a classic xref table at the current parser position.
// p->pos should point at "xref"
// *out_trailer receives the trailer dict for this specific xref section.
static TspdfError parse_classic_xref(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **out_trailer) {
    // Skip "xref"
    if (p->pos + 4 > p->len || memcmp(p->data + p->pos, "xref", 4) != 0) {
        return TSPDF_ERR_XREF;
    }
    p->pos += 4;
    tspdf_skip_whitespace(p);

    // Parse subsections
    while (p->pos < p->len) {
        // Check if we hit "trailer"
        if (p->pos + 7 <= p->len && memcmp(p->data + p->pos, "trailer", 7) == 0) {
            break;
        }

        // Parse first_obj count
        size_t start = p->pos;
        while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
        if (p->pos == start) return TSPDF_ERR_XREF;
        char buf[32];
        size_t nlen = p->pos - start;
        if (nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
        memcpy(buf, p->data + start, nlen);
        buf[nlen] = '\0';
        size_t first_obj = 0;
        if (!parse_decimal_size(buf, &first_obj)) return TSPDF_ERR_XREF;

        tspdf_skip_whitespace(p);

        start = p->pos;
        while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
        if (p->pos == start) return TSPDF_ERR_XREF;
        nlen = p->pos - start;
        if (nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
        memcpy(buf, p->data + start, nlen);
        buf[nlen] = '\0';
        size_t entry_count = 0;
        if (!parse_decimal_size(buf, &entry_count)) return TSPDF_ERR_XREF;

        // Skip to next line
        while (p->pos < p->len && is_xref_inline_ws(p->data[p->pos])) {
            p->pos++;
        }
        if (p->pos < p->len && p->data[p->pos] == '\r') p->pos++;
        if (p->pos < p->len && p->data[p->pos] == '\n') p->pos++;

        // Ensure capacity
        size_t needed;
        if (!checked_add_size(first_obj, entry_count, &needed)) {
            return TSPDF_ERR_XREF;
        }
        if (needed > max_plausible_objects(p->len)) {
            return TSPDF_ERR_XREF;
        }
        if (!ensure_xref_capacity(xref, needed, p->arena)) {
            return TSPDF_ERR_ALLOC;
        }

        // Parse entries (20-byte lines: "OOOOOOOOOO GGGGG n \n" or "f \n")
        for (size_t i = 0; i < entry_count; i++) {
            // Each entry is exactly 20 bytes: 10 offset + ' ' + 5 gen + ' ' + type + ' ' + eol
            // But we parse more flexibly
            tspdf_skip_whitespace(p);

            // Parse offset (10 digits)
            start = p->pos;
            while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
            nlen = p->pos - start;
            if (nlen == 0 || nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
            memcpy(buf, p->data + start, nlen);
            buf[nlen] = '\0';
            size_t offset = 0;
            if (!parse_decimal_size(buf, &offset)) return TSPDF_ERR_XREF;

            // Skip inline whitespace
            while (p->pos < p->len && is_xref_inline_ws(p->data[p->pos])) p->pos++;

            // Parse gen (5 digits)
            start = p->pos;
            while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
            nlen = p->pos - start;
            if (nlen == 0 || nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
            memcpy(buf, p->data + start, nlen);
            buf[nlen] = '\0';
            uint64_t gen_value = 0;
            if (!parse_decimal_u64(buf, &gen_value) || gen_value > UINT16_MAX) {
                return TSPDF_ERR_XREF;
            }
            uint16_t gen = (uint16_t)gen_value;

            // Skip inline whitespace
            while (p->pos < p->len && is_xref_inline_ws(p->data[p->pos])) p->pos++;

            // Parse type: 'n' or 'f'
            if (p->pos >= p->len) return TSPDF_ERR_XREF;
            uint8_t type_ch = p->data[p->pos++];
            if (type_ch != 'n' && type_ch != 'f') {
                return TSPDF_ERR_XREF;
            }

            size_t idx;
            if (!checked_add_size(first_obj, i, &idx)) {
                return TSPDF_ERR_XREF;
            }
            if (!xref->entries[idx].seen) {
                xref->entries[idx].offset = offset;
                xref->entries[idx].gen = gen;
                xref->entries[idx].in_use = (type_ch == 'n');
                xref->entries[idx].compressed = false;
                xref->entries[idx].seen = true;
            }

            // Skip rest of line (space + eol chars)
            while (p->pos < p->len && (is_xref_inline_ws(p->data[p->pos]) ||
                                         p->data[p->pos] == '\r' ||
                                         p->data[p->pos] == '\n')) {
                p->pos++;
            }
        }
    }

    // Parse trailer
    if (p->pos + 7 > p->len || memcmp(p->data + p->pos, "trailer", 7) != 0) {
        return TSPDF_ERR_XREF;
    }
    p->pos += 7;
    tspdf_skip_whitespace(p);

    TspdfObj *trailer = tspdf_parse_obj(p);
    if (!trailer || trailer->type != TSPDF_OBJ_DICT) {
        return TSPDF_ERR_XREF;
    }

    // Only set main trailer if we don't already have one (newer xref takes priority)
    if (!xref->trailer) {
        xref->trailer = trailer;
    }

    *out_trailer = trailer;
    return TSPDF_OK;
}

// Parse xref stream (PDF 1.5+)
static TspdfError parse_xref_stream(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **out_trailer) {
    uint32_t num;
    uint16_t gen;
    TspdfObj *obj = tspdf_parse_indirect_obj(p, &num, &gen);
    if (!obj || obj->type != TSPDF_OBJ_STREAM) {
        return TSPDF_ERR_XREF;
    }

    TspdfObj *stream_dict = obj->stream.dict;

    // Some producers omit /Type on xref streams; /W and /Size below still prove the shape.
    TspdfObj *type_val = tspdf_dict_get(stream_dict, "Type");
    if (type_val &&
        (type_val->type != TSPDF_OBJ_NAME ||
         strcmp((const char *)type_val->string.data, "XRef") != 0)) {
        return TSPDF_ERR_XREF;
    }

    // Get /W array (field widths)
    TspdfObj *w_obj = tspdf_dict_get(stream_dict, "W");
    if (!w_obj || w_obj->type != TSPDF_OBJ_ARRAY || w_obj->array.count != 3) {
        return TSPDF_ERR_XREF;
    }
    int w[3];
    for (int i = 0; i < 3; i++) {
        if (w_obj->array.items[i].type != TSPDF_OBJ_INT) return TSPDF_ERR_XREF;
        if (w_obj->array.items[i].integer < 0 || w_obj->array.items[i].integer > INT_MAX) {
            return TSPDF_ERR_XREF;
        }
        if (w_obj->array.items[i].integer > 8) {
            return TSPDF_ERR_XREF;
        }
        w[i] = (int)w_obj->array.items[i].integer;
    }
    if (w[0] > INT_MAX - w[1] || w[0] + w[1] > INT_MAX - w[2]) {
        return TSPDF_ERR_XREF;
    }
    int entry_size = w[0] + w[1] + w[2];
    if (entry_size <= 0) return TSPDF_ERR_XREF;

    // Get /Size
    TspdfObj *size_obj = tspdf_dict_get(stream_dict, "Size");
    if (!size_obj || size_obj->type != TSPDF_OBJ_INT) return TSPDF_ERR_XREF;
    if (size_obj->integer < 0) return TSPDF_ERR_XREF;
    size_t total_objects = (size_t)size_obj->integer;
    if (total_objects > max_plausible_objects(p->len)) return TSPDF_ERR_XREF;

    const uint8_t *raw_data = p->data + obj->stream.raw_offset;
    size_t raw_len = obj->stream.raw_len;

    uint8_t *decompressed = NULL;
    size_t decompressed_len = 0;
    TspdfError decode_err = decode_stream_data(stream_dict, raw_data, raw_len,
                                               &decompressed, &decompressed_len);
    if (decode_err != TSPDF_OK) {
        return decode_err;
    }

    // Get /Index array (default: [0 TspdfSize])
    size_t *subsections = NULL;
    size_t num_subsections = 0;
    TspdfObj *index_obj = tspdf_dict_get(stream_dict, "Index");
    if (index_obj) {
        if (index_obj->type != TSPDF_OBJ_ARRAY) {
            free(decompressed);
            return TSPDF_ERR_XREF;
        }
        if (index_obj->array.count == 0 || (index_obj->array.count % 2) != 0) {
            free(decompressed);
            return TSPDF_ERR_XREF;
        }
        num_subsections = index_obj->array.count / 2;
        subsections = (size_t *)tspdf_arena_alloc(p->arena, sizeof(size_t) * index_obj->array.count);
        if (!subsections) {
            free(decompressed);
            return TSPDF_ERR_ALLOC;
        }
        for (size_t i = 0; i < index_obj->array.count; i++) {
            if (index_obj->array.items[i].type != TSPDF_OBJ_INT ||
                index_obj->array.items[i].integer < 0) {
                free(decompressed);
                return TSPDF_ERR_XREF;
            }
            subsections[i] = (size_t)index_obj->array.items[i].integer;
        }
    } else {
        num_subsections = 1;
        subsections = (size_t *)tspdf_arena_alloc(p->arena, sizeof(size_t) * 2);
        if (!subsections) {
            free(decompressed);
            return TSPDF_ERR_ALLOC;
        }
        subsections[0] = 0;
        subsections[1] = total_objects;
    }

    for (size_t s = 0; s < num_subsections; s++) {
        size_t first = subsections[s * 2];
        size_t count = subsections[s * 2 + 1];
        if (count > SIZE_MAX - first || first + count > total_objects) {
            free(decompressed);
            return TSPDF_ERR_XREF;
        }
    }

    // Ensure capacity
    if (total_objects > SIZE_MAX / sizeof(TspdfReaderXrefEntry)) {
        free(decompressed);
        return TSPDF_ERR_XREF;
    }
    if (!ensure_xref_capacity(xref, total_objects, p->arena)) {
        free(decompressed);
        return TSPDF_ERR_ALLOC;
    }

    // Unpack entries
    size_t data_pos = 0;
    for (size_t s = 0; s < num_subsections; s++) {
        size_t first = subsections[s * 2];
        size_t count = subsections[s * 2 + 1];
        size_t subsection_end;
        if (!checked_add_size(first, count, &subsection_end)) {
            free(decompressed);
            return TSPDF_ERR_XREF;
        }
        (void)subsection_end;

        for (size_t i = 0; i < count; i++) {
            if (data_pos > decompressed_len || (size_t)entry_size > decompressed_len - data_pos) {
                free(decompressed);
                return TSPDF_ERR_XREF;
            }

            // Read fields
            uint64_t fields[3] = {0, 0, 0};
            for (int f = 0; f < 3; f++) {
                for (int b = 0; b < w[f]; b++) {
                    fields[f] = (fields[f] << 8) | decompressed[data_pos++];
                }
            }

            // Default: if w[0] == 0, type defaults to 1
            uint64_t type = (w[0] == 0) ? 1 : fields[0];
            size_t idx;
            if (!checked_add_size(first, i, &idx)) {
                free(decompressed);
                return TSPDF_ERR_XREF;
            }
            if (idx >= xref->count) continue;
            if (type > 2) {
                free(decompressed);
                return TSPDF_ERR_XREF;
            }

            if (!xref->entries[idx].seen) {
                xref->entries[idx].seen = true;
                switch (type) {
                    case 0: // free
                        if (!uint64_fits_size_t(fields[1]) || fields[2] > UINT16_MAX) {
                            free(decompressed);
                            return TSPDF_ERR_XREF;
                        }
                        xref->entries[idx].in_use = false;
                        xref->entries[idx].offset = (size_t)fields[1];
                        xref->entries[idx].gen = (uint16_t)fields[2];
                        break;
                    case 1: // normal (uncompressed)
                        if (!uint64_fits_size_t(fields[1]) || fields[2] > UINT16_MAX) {
                            free(decompressed);
                            return TSPDF_ERR_XREF;
                        }
                        xref->entries[idx].in_use = true;
                        xref->entries[idx].compressed = false;
                        xref->entries[idx].offset = (size_t)fields[1];
                        xref->entries[idx].gen = (uint16_t)fields[2];
                        break;
                    case 2: // compressed in object stream
                        if (fields[1] > UINT32_MAX || !uint64_fits_size_t(fields[2])) {
                            free(decompressed);
                            return TSPDF_ERR_XREF;
                        }
                        xref->entries[idx].in_use = true;
                        xref->entries[idx].compressed = true;
                        xref->entries[idx].stream_obj = (uint32_t)fields[1];
                        xref->entries[idx].stream_idx = (size_t)fields[2];
                        break;
                }
            }
        }
    }

    free(decompressed);

    // The stream dict itself serves as the trailer
    if (!xref->trailer) {
        xref->trailer = stream_dict;
    }

    *out_trailer = stream_dict;
    return TSPDF_OK;
}

#define XREF_REPAIR_WINDOW 1024
#define STARTXREF_RETRY_LIMIT 64

typedef struct {
    TspdfReaderXrefEntry *entries;
    TspdfReaderXrefEntry *entries_copy;
    size_t count;
    TspdfObj *trailer;
    size_t parser_pos;
} XrefParseSnapshot;

static bool xref_snapshot_capture(TspdfParser *p, TspdfReaderXref *xref,
                                  XrefParseSnapshot *snapshot) {
    snapshot->entries = xref->entries;
    snapshot->entries_copy = NULL;
    snapshot->count = xref->count;
    snapshot->trailer = xref->trailer;
    snapshot->parser_pos = p->pos;

    if (xref->entries && xref->count > 0) {
        snapshot->entries_copy = (TspdfReaderXrefEntry *)malloc(sizeof(TspdfReaderXrefEntry) * xref->count);
        if (!snapshot->entries_copy) {
            return false;
        }
        memcpy(snapshot->entries_copy, xref->entries, sizeof(TspdfReaderXrefEntry) * xref->count);
    }

    return true;
}

static void xref_snapshot_restore(TspdfParser *p, TspdfReaderXref *xref,
                                  const XrefParseSnapshot *snapshot) {
    xref->entries = snapshot->entries;
    xref->count = snapshot->count;
    xref->trailer = snapshot->trailer;
    p->pos = snapshot->parser_pos;

    if (snapshot->entries && snapshot->entries_copy && snapshot->count > 0) {
        memcpy(snapshot->entries, snapshot->entries_copy,
               sizeof(TspdfReaderXrefEntry) * snapshot->count);
    }
}

static void xref_snapshot_destroy(XrefParseSnapshot *snapshot) {
    free(snapshot->entries_copy);
    snapshot->entries_copy = NULL;
}

static bool looks_like_indirect_obj_at(const uint8_t *data, size_t len, size_t offset) {
    size_t pos = offset;
    if (pos >= len || !isdigit(data[pos])) {
        return false;
    }

    while (pos < len && isdigit(data[pos])) pos++;
    if (pos >= len || !(data[pos] == ' ' || data[pos] == '\t' ||
                        data[pos] == '\r' || data[pos] == '\n' ||
                        data[pos] == '\0' || data[pos] == '\f')) {
        return false;
    }
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                         data[pos] == '\r' || data[pos] == '\n' ||
                         data[pos] == '\0' || data[pos] == '\f')) {
        pos++;
    }

    if (pos >= len || !isdigit(data[pos])) {
        return false;
    }
    while (pos < len && isdigit(data[pos])) pos++;
    if (pos >= len || !(data[pos] == ' ' || data[pos] == '\t' ||
                        data[pos] == '\r' || data[pos] == '\n' ||
                        data[pos] == '\0' || data[pos] == '\f')) {
        return false;
    }
    while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                         data[pos] == '\r' || data[pos] == '\n' ||
                         data[pos] == '\0' || data[pos] == '\f')) {
        pos++;
    }

    return pos + 3 <= len && memcmp(data + pos, "obj", 3) == 0;
}

static bool looks_like_xref_candidate_at(TspdfParser *p, size_t offset) {
    size_t pos = offset;
    while (pos < p->len && (p->data[pos] == ' ' || p->data[pos] == '\t' ||
                            p->data[pos] == '\r' || p->data[pos] == '\n' ||
                            p->data[pos] == '\0' || p->data[pos] == '\f')) {
        pos++;
    }

    if (pos + 4 <= p->len && memcmp(p->data + pos, "xref", 4) == 0) {
        return true;
    }

    return looks_like_indirect_obj_at(p->data, p->len, pos);
}

static bool parse_nearby_xref_stream(TspdfParser *p, TspdfReaderXref *xref,
                                     size_t declared_offset,
                                     const XrefParseSnapshot *snapshot,
                                     TspdfObj **out_trailer,
                                     TspdfError *out_err) {
    for (size_t delta = 1; delta <= XREF_REPAIR_WINDOW; delta++) {
        size_t candidates[2];
        size_t candidate_count = 0;

        if (declared_offset >= delta) {
            candidates[candidate_count++] = declared_offset - delta;
        }
        if (declared_offset <= p->len && delta <= p->len - declared_offset) {
            candidates[candidate_count++] = declared_offset + delta;
        }

        for (size_t i = 0; i < candidate_count; i++) {
            size_t candidate = candidates[i];
            while (candidate < p->len && (p->data[candidate] == ' ' ||
                                          p->data[candidate] == '\t' ||
                                          p->data[candidate] == '\r' ||
                                          p->data[candidate] == '\n' ||
                                          p->data[candidate] == '\0' ||
                                          p->data[candidate] == '\f')) {
                candidate++;
            }
            if (!looks_like_indirect_obj_at(p->data, p->len, candidate)) {
                continue;
            }

            p->pos = candidate;
            TspdfError err = parse_xref_stream(p, xref, out_trailer);
            if (err == TSPDF_OK) {
                if (out_err) *out_err = TSPDF_OK;
                return true;
            }

            xref_snapshot_restore(p, xref, snapshot);
            if (out_err) *out_err = err;
        }
    }

    return false;
}

static TspdfError parse_xref_section_at(TspdfParser *p, TspdfReaderXref *xref,
                                        size_t xref_offset, TspdfObj **out_trailer) {
    if (xref_offset >= p->len) return TSPDF_ERR_XREF;

    p->pos = xref_offset;
    tspdf_skip_whitespace(p);

    TspdfObj *current_trailer = NULL;
    TspdfError err;
    bool parsed_classic = false;
    if (p->pos + 4 <= p->len && memcmp(p->data + p->pos, "xref", 4) == 0) {
        err = parse_classic_xref(p, xref, &current_trailer);
        parsed_classic = true;
    } else {
        // Might be an xref stream (starts with object number)
        err = parse_xref_stream(p, xref, &current_trailer);
    }

    if (err != TSPDF_OK) return err;

    if (parsed_classic) {
        TspdfObj *xrefstm = tspdf_dict_get(current_trailer, "XRefStm");
        if (xrefstm) {
            if (xrefstm->type != TSPDF_OBJ_INT || xrefstm->integer < 0) {
                return TSPDF_ERR_XREF;
            }
            if ((uint64_t)xrefstm->integer > SIZE_MAX) {
                return TSPDF_ERR_XREF;
            }

            size_t saved_pos = p->pos;
            TspdfObj *stream_trailer = NULL;
            XrefParseSnapshot xrefstm_snapshot;
            if (!xref_snapshot_capture(p, xref, &xrefstm_snapshot)) {
                return TSPDF_ERR_ALLOC;
            }

            p->pos = (size_t)xrefstm->integer;
            err = parse_xref_stream(p, xref, &stream_trailer);
            if (err != TSPDF_OK) {
                xref_snapshot_restore(p, xref, &xrefstm_snapshot);
                TspdfError repair_err = err;
                if (!parse_nearby_xref_stream(p, xref, (size_t)xrefstm->integer,
                                              &xrefstm_snapshot, &stream_trailer,
                                              &repair_err)) {
                    xref_snapshot_destroy(&xrefstm_snapshot);
                    p->pos = saved_pos;
                    return repair_err;
                }
            }

            xref_snapshot_destroy(&xrefstm_snapshot);
            p->pos = saved_pos;
        }
    }

    *out_trailer = current_trailer;
    return TSPDF_OK;
}

static TspdfDictEntry *dict_find_entry(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) {
        return NULL;
    }

    for (size_t i = dict->dict.count; i > 0; i--) {
        size_t idx = i - 1;
        if (strcmp(dict->dict.entries[idx].key, key) == 0) {
            return &dict->dict.entries[idx];
        }
    }

    return NULL;
}

static TspdfError inherit_trailer_entry(TspdfParser *p, TspdfObj *dst,
                                        TspdfObj *src, const char *key) {
    if (!dst || dst->type != TSPDF_OBJ_DICT || !src || src->type != TSPDF_OBJ_DICT) {
        return TSPDF_OK;
    }
    if (dict_find_entry(dst, key)) {
        return TSPDF_OK;
    }

    TspdfDictEntry *src_entry = dict_find_entry(src, key);
    if (!src_entry) {
        return TSPDF_OK;
    }

    size_t new_count = dst->dict.count + 1;
    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(
        p->arena, sizeof(TspdfDictEntry) * new_count);
    if (!entries) {
        return TSPDF_ERR_ALLOC;
    }

    if (dst->dict.count > 0) {
        memcpy(entries, dst->dict.entries, sizeof(TspdfDictEntry) * dst->dict.count);
    }
    entries[dst->dict.count] = *src_entry;
    dst->dict.entries = entries;
    dst->dict.count = new_count;
    return TSPDF_OK;
}

static TspdfError inherit_stable_trailer_entries(TspdfParser *p, TspdfReaderXref *xref,
                                                 TspdfObj *older_trailer) {
    if (!xref->trailer || xref->trailer == older_trailer) {
        return TSPDF_OK;
    }

    const char *keys[] = {"Root", "Encrypt", "Info", "ID"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        TspdfError err = inherit_trailer_entry(p, xref->trailer, older_trailer, keys[i]);
        if (err != TSPDF_OK) {
            return err;
        }
    }

    return TSPDF_OK;
}

static bool parse_nearby_xref_section(TspdfParser *p, TspdfReaderXref *xref,
                                      size_t declared_offset,
                                      const XrefParseSnapshot *snapshot,
                                      TspdfObj **out_trailer,
                                      TspdfError *out_err) {
    for (size_t delta = 1; delta <= XREF_REPAIR_WINDOW; delta++) {
        size_t candidates[2];
        size_t candidate_count = 0;

        if (declared_offset >= delta) {
            candidates[candidate_count++] = declared_offset - delta;
        }
        if (declared_offset <= p->len && delta <= p->len - declared_offset) {
            candidates[candidate_count++] = declared_offset + delta;
        }

        for (size_t i = 0; i < candidate_count; i++) {
            size_t candidate = candidates[i];
            if (!looks_like_xref_candidate_at(p, candidate)) {
                continue;
            }

            TspdfError err = parse_xref_section_at(p, xref, candidate, out_trailer);
            if (err == TSPDF_OK) {
                if (out_err) *out_err = TSPDF_OK;
                return true;
            }

            xref_snapshot_restore(p, xref, snapshot);
            if (out_err) *out_err = err;
        }
    }

    return false;
}

static TspdfError record_prev_xref_offset(TspdfParser *p, size_t **offsets,
                                          size_t *count, size_t *capacity,
                                          size_t offset) {
    for (size_t i = 0; i < *count; i++) {
        if ((*offsets)[i] == offset) {
            return TSPDF_ERR_XREF;
        }
    }

    if (*count >= *capacity) {
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(size_t)) {
            return TSPDF_ERR_XREF;
        }

        size_t *new_offsets = (size_t *)tspdf_arena_alloc(p->arena,
                                                          sizeof(size_t) * new_capacity);
        if (!new_offsets) {
            return TSPDF_ERR_ALLOC;
        }
        if (*offsets && *count > 0) {
            memcpy(new_offsets, *offsets, sizeof(size_t) * *count);
        }
        *offsets = new_offsets;
        *capacity = new_capacity;
    }

    (*offsets)[(*count)++] = offset;
    return TSPDF_OK;
}

static TspdfError parse_xref_from_offset(TspdfParser *p, TspdfReaderXref *xref, size_t xref_offset) {
    size_t *visited_offsets = NULL;
    size_t visited_count = 0;
    size_t visited_capacity = 0;

    // Follow /Prev chain from the given offset
    while (1) {
        TspdfError err = record_prev_xref_offset(p, &visited_offsets, &visited_count,
                                                 &visited_capacity, xref_offset);
        if (err != TSPDF_OK) {
            return err;
        }

        XrefParseSnapshot snapshot;
        if (!xref_snapshot_capture(p, xref, &snapshot)) {
            return TSPDF_ERR_ALLOC;
        }

        bool declared_looks_plausible = looks_like_xref_candidate_at(p, xref_offset);
        TspdfObj *current_trailer = NULL;
        err = parse_xref_section_at(p, xref, xref_offset, &current_trailer);
        if (err != TSPDF_OK) {
            xref_snapshot_restore(p, xref, &snapshot);
            if (!declared_looks_plausible &&
                parse_nearby_xref_section(p, xref, xref_offset, &snapshot,
                                          &current_trailer, &err)) {
                // repaired successfully
            } else {
                xref_snapshot_destroy(&snapshot);
                return err;
            }
        }
        xref_snapshot_destroy(&snapshot);

        err = inherit_stable_trailer_entries(p, xref, current_trailer);
        if (err != TSPDF_OK) {
            return err;
        }

        // Check for /Prev (incremental updates)
        TspdfObj *prev = tspdf_dict_get(current_trailer, "Prev");
        if (prev && prev->type == TSPDF_OBJ_INT) {
            if (prev->integer < 0) {
                return TSPDF_ERR_XREF;
            }
            xref_offset = (size_t)prev->integer;
        } else {
            break;
        }
    }

    return TSPDF_OK;
}

static bool is_xref_recovery_boundary(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\0' || c == '\f';
}

static bool looks_like_classic_xref_keyword_at(const uint8_t *data, size_t len, size_t offset) {
    if (offset + 4 > len || memcmp(data + offset, "xref", 4) != 0) {
        return false;
    }
    if (offset > 0 && !is_xref_recovery_boundary(data[offset - 1])) {
        return false;
    }
    if (offset + 4 < len && !is_xref_recovery_boundary(data[offset + 4])) {
        return false;
    }
    return true;
}

static TspdfError parse_last_classic_xref_without_startxref(TspdfParser *p,
                                                            TspdfReaderXref *xref) {
    if (!p || !p->data || p->len < 4) {
        return TSPDF_ERR_XREF;
    }

    TspdfError last_err = TSPDF_ERR_XREF;
    for (size_t cursor = p->len - 3; cursor > 0; cursor--) {
        size_t candidate = cursor - 1;
        if (!looks_like_classic_xref_keyword_at(p->data, p->len, candidate)) {
            continue;
        }

        XrefParseSnapshot snapshot;
        if (!xref_snapshot_capture(p, xref, &snapshot)) {
            return TSPDF_ERR_ALLOC;
        }

        TspdfError err = parse_xref_from_offset(p, xref, candidate);
        if (err == TSPDF_OK) {
            xref_snapshot_destroy(&snapshot);
            return TSPDF_OK;
        }

        xref_snapshot_restore(p, xref, &snapshot);
        xref_snapshot_destroy(&snapshot);
        last_err = err;
    }

    return last_err;
}

// Defined later; used by the object-scan reconstruction to identify the
// catalog object.
static TspdfObj *parse_expected_indirect_obj_at(TspdfParser *p, size_t offset,
                                                uint32_t expected_num,
                                                uint16_t expected_gen,
                                                TspdfError *out_error);

// Read the "N G obj" header at `offset` (already known to look like one) and
// return the object/generation numbers. Returns false on malformed input.
static bool read_obj_header_at(const uint8_t *data, size_t len, size_t offset,
                               uint32_t *out_num, uint16_t *out_gen) {
    size_t pos = offset;

    size_t start = pos;
    while (pos < len && isdigit(data[pos])) pos++;
    if (pos == start || pos - start >= 16) return false;
    char buf[16];
    memcpy(buf, data + start, pos - start);
    buf[pos - start] = '\0';
    uint64_t num64 = 0;
    if (!parse_decimal_u64(buf, &num64) || num64 > UINT32_MAX) return false;

    while (pos < len && is_xref_recovery_boundary(data[pos])) pos++;

    start = pos;
    while (pos < len && isdigit(data[pos])) pos++;
    if (pos == start || pos - start >= 16) return false;
    memcpy(buf, data + start, pos - start);
    buf[pos - start] = '\0';
    uint64_t gen64 = 0;
    if (!parse_decimal_u64(buf, &gen64) || gen64 > UINT16_MAX) return false;

    *out_num = (uint32_t)num64;
    *out_gen = (uint16_t)gen64;
    return true;
}

// Last-resort reconstruction for files whose xref/trailer are unusable: scan
// the whole buffer for "N G obj" markers, build a synthetic xref (last
// definition of an object number wins), then locate the catalog. The Root is
// found by parsing each object and taking the one whose dict is
// /Type /Catalog; if none is found we fall back to scanning for a "/Root N G R"
// reference. A synthetic trailer dict carrying /Root (and /Encrypt if present)
// is installed so the normal open path can proceed. Bounded by the buffer size.
static TspdfError reconstruct_by_object_scan(TspdfParser *p, TspdfReaderXref *xref) {
    if (!p || !p->data || p->len < 4) {
        return TSPDF_ERR_XREF;
    }

    // Pass 1: record every "N G obj" definition (last wins) and the highest
    // object number seen.
    size_t max_num = 0;
    bool any = false;
    for (size_t i = 0; i + 3 < p->len; i++) {
        // A marker must start at a token boundary to avoid matching digits
        // inside another number.
        if (i > 0 && !is_xref_recovery_boundary(p->data[i - 1])) {
            continue;
        }
        if (!looks_like_indirect_obj_at(p->data, p->len, i)) {
            continue;
        }
        uint32_t num;
        uint16_t gen;
        if (!read_obj_header_at(p->data, p->len, i, &num, &gen)) {
            continue;
        }
        if (num > max_num) max_num = num;
        any = true;
    }

    if (!any || max_num > max_plausible_objects(p->len)) {
        return TSPDF_ERR_XREF;
    }

    size_t needed = max_num + 1;
    if (!ensure_xref_capacity(xref, needed, p->arena)) {
        return TSPDF_ERR_ALLOC;
    }

    // Pass 2: fill entries; later definitions override earlier ones.
    for (size_t i = 0; i + 3 < p->len; i++) {
        if (i > 0 && !is_xref_recovery_boundary(p->data[i - 1])) {
            continue;
        }
        if (!looks_like_indirect_obj_at(p->data, p->len, i)) {
            continue;
        }
        uint32_t num;
        uint16_t gen;
        if (!read_obj_header_at(p->data, p->len, i, &num, &gen)) {
            continue;
        }
        if (num >= xref->count) continue;
        xref->entries[num].offset = i;
        xref->entries[num].gen = gen;
        xref->entries[num].in_use = true;
        xref->entries[num].compressed = false;
        xref->entries[num].seen = true;
    }

    // Locate the catalog: parse each in-use object and look for /Type /Catalog.
    uint32_t root_num = 0;
    bool found_root = false;
    for (size_t num = 0; num < xref->count && !found_root; num++) {
        if (!xref->entries[num].in_use) continue;

        TspdfError perr = TSPDF_OK;
        TspdfObj *obj = parse_expected_indirect_obj_at(p, xref->entries[num].offset,
                                                       (uint32_t)num,
                                                       xref->entries[num].gen, &perr);
        if (!obj) continue;
        TspdfObj *dict = (obj->type == TSPDF_OBJ_DICT) ? obj :
                         (obj->type == TSPDF_OBJ_STREAM) ? obj->stream.dict : NULL;
        if (!dict) continue;
        TspdfObj *type = tspdf_dict_get(dict, "Type");
        if (type && type->type == TSPDF_OBJ_NAME &&
            strcmp((const char *)type->string.data, "Catalog") == 0) {
            root_num = (uint32_t)num;
            found_root = true;
        }
    }

    if (!found_root) {
        return TSPDF_ERR_XREF;
    }

    // Build a synthetic trailer: << /Root root_num 0 R >>. If an /Encrypt dict
    // object exists, wire it in too so encrypted-but-damaged files still work.
    TspdfObj *root_ref = (TspdfObj *)tspdf_arena_alloc_zero(p->arena, sizeof(TspdfObj));
    if (!root_ref) return TSPDF_ERR_ALLOC;
    root_ref->type = TSPDF_OBJ_REF;
    root_ref->ref.num = root_num;
    root_ref->ref.gen = xref->entries[root_num].gen;

    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(p->arena,
                                  2 * sizeof(TspdfDictEntry));
    if (!entries) return TSPDF_ERR_ALLOC;
    size_t entry_count = 0;
    entries[entry_count].key = "Root";
    entries[entry_count].value = root_ref;
    entry_count++;

    TspdfObj *trailer = (TspdfObj *)tspdf_arena_alloc_zero(p->arena, sizeof(TspdfObj));
    if (!trailer) return TSPDF_ERR_ALLOC;
    trailer->type = TSPDF_OBJ_DICT;
    trailer->dict.entries = entries;
    trailer->dict.count = entry_count;

    xref->trailer = trailer;
    return TSPDF_OK;
}

TspdfError tspdf_xref_reconstruct_by_scan(TspdfParser *p, TspdfReaderXref *xref) {
    if (!p || !xref) return TSPDF_ERR_XREF;
    xref->entries = NULL;
    xref->count = 0;
    xref->trailer = NULL;
    return reconstruct_by_object_scan(p, xref);
}

TspdfError tspdf_xref_parse(TspdfParser *p, TspdfReaderXref *xref) {
    TspdfError last_err = TSPDF_ERR_XREF;

    for (int skip = 0; skip < STARTXREF_RETRY_LIMIT; skip++) {
        size_t xref_offset = 0;
        if (!find_startxref_nth(p->data, p->len, &xref_offset, skip)) {
            break;
        }

        XrefParseSnapshot snapshot;
        if (!xref_snapshot_capture(p, xref, &snapshot)) {
            return TSPDF_ERR_ALLOC;
        }

        TspdfError err = parse_xref_from_offset(p, xref, xref_offset);
        if (err == TSPDF_OK) {
            xref_snapshot_destroy(&snapshot);
            return TSPDF_OK;
        }

        xref_snapshot_restore(p, xref, &snapshot);
        xref_snapshot_destroy(&snapshot);
        last_err = err;
    }

    TspdfError recovery_err = parse_last_classic_xref_without_startxref(p, xref);
    if (recovery_err == TSPDF_OK) {
        return TSPDF_OK;
    }

    // Last resort: rebuild the xref by scanning the whole buffer for object
    // markers. Do this from a clean snapshot so partial state from the failed
    // recovery attempts above does not leak into the reconstructed table.
    {
        XrefParseSnapshot snapshot;
        if (!xref_snapshot_capture(p, xref, &snapshot)) {
            return TSPDF_ERR_ALLOC;
        }
        xref->entries = NULL;
        xref->count = 0;
        xref->trailer = NULL;

        TspdfError scan_err = reconstruct_by_object_scan(p, xref);
        if (scan_err == TSPDF_OK) {
            xref_snapshot_destroy(&snapshot);
            return TSPDF_OK;
        }

        xref_snapshot_restore(p, xref, &snapshot);
        xref_snapshot_destroy(&snapshot);
    }

    return last_err != TSPDF_OK ? last_err : recovery_err;
}

// Recursively decrypt all strings in an object tree
static void decrypt_obj_strings(TspdfCrypt *crypt, TspdfObj *obj, uint32_t obj_num, uint16_t gen) {
    if (!obj) return;
    switch (obj->type) {
        case TSPDF_OBJ_STRING:
            tspdf_crypt_decrypt_string(crypt, obj_num, gen, obj->string.data, &obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++)
                decrypt_obj_strings(crypt, &obj->array.items[i], obj_num, gen);
            break;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++)
                decrypt_obj_strings(crypt, obj->dict.entries[i].value, obj_num, gen);
            break;
        case TSPDF_OBJ_STREAM:
            // Decrypt strings in the stream dict (but not stream data here)
            decrypt_obj_strings(crypt, obj->stream.dict, obj_num, gen);
            break;
        default:
            break;
    }
}

// Check if an object is the /Encrypt dict (should not be decrypted)
static bool is_encrypt_dict_obj(TspdfReaderXref *xref, uint32_t obj_num) {
    if (!xref->trailer) return false;
    TspdfObj *encrypt = tspdf_dict_get(xref->trailer, "Encrypt");
    if (!encrypt) return false;
    if (encrypt->type == TSPDF_OBJ_REF) return encrypt->ref.num == obj_num;
    return false;
}

static bool stream_len_points_to_endstream(const uint8_t *data, size_t data_len,
                                           size_t raw_offset, size_t raw_len) {
    if (raw_offset > data_len) {
        return false;
    }
    if (raw_len > data_len - raw_offset) {
        return false;
    }

    size_t pos = raw_offset + raw_len;
    while (pos < data_len && (data[pos] == ' ' || data[pos] == '\t' ||
                              data[pos] == '\r' || data[pos] == '\n' ||
                              data[pos] == '\0' || data[pos] == '\f')) {
        pos++;
    }

    const char *needle = "endstream";
    size_t needle_len = 9;
    return pos + needle_len <= data_len && memcmp(data + pos, needle, needle_len) == 0;
}

static void resolve_indirect_stream_length(TspdfReaderXref *xref, TspdfParser *p,
                                           uint32_t obj_num, TspdfObj **cache,
                                           TspdfCrypt *crypt, TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_STREAM) {
        return;
    }

    TspdfObj *len_obj = tspdf_dict_get(obj->stream.dict, "Length");
    if (!len_obj || len_obj->type != TSPDF_OBJ_REF || len_obj->ref.num == obj_num) {
        return;
    }

    TspdfObj *resolved = tspdf_xref_resolve(xref, p, len_obj->ref.num, cache, crypt);
    if (!resolved || resolved->type != TSPDF_OBJ_INT || resolved->integer < 0) {
        return;
    }

    size_t raw_len = (size_t)resolved->integer;
    if (stream_len_points_to_endstream(p->data, p->len, obj->stream.raw_offset, raw_len)) {
        obj->stream.raw_len = raw_len;
    }
}

static TspdfObj *parse_expected_indirect_obj_at(TspdfParser *p, size_t offset,
                                                uint32_t expected_num,
                                                uint16_t expected_gen,
                                                TspdfError *out_error) {
    if (offset >= p->len) {
        if (out_error) *out_error = TSPDF_ERR_PARSE;
        return NULL;
    }

    size_t saved_pos = p->pos;
    TspdfError saved_error = p->error;
    p->pos = offset;
    p->error = TSPDF_OK;

    uint32_t num = 0;
    uint16_t gen = 0;
    TspdfObj *obj = tspdf_parse_indirect_obj(p, &num, &gen);
    TspdfError parse_error = p->error;
    p->pos = saved_pos;

    if (obj && num == expected_num && gen == expected_gen) {
        p->error = saved_error;
        if (out_error) *out_error = TSPDF_OK;
        return obj;
    }

    if (parse_error == TSPDF_ERR_ALLOC) {
        p->error = TSPDF_ERR_ALLOC;
        if (out_error) *out_error = TSPDF_ERR_ALLOC;
    } else {
        p->error = saved_error;
        if (out_error) *out_error = TSPDF_ERR_PARSE;
    }
    return NULL;
}

static TspdfObj *parse_expected_indirect_obj_near(TspdfParser *p, size_t declared_offset,
                                                  uint32_t expected_num,
                                                  uint16_t expected_gen,
                                                  size_t *out_repaired_offset,
                                                  TspdfError *out_error) {
    for (size_t delta = 1; delta <= XREF_REPAIR_WINDOW; delta++) {
        size_t candidates[2];
        size_t candidate_count = 0;

        if (declared_offset >= delta) {
            candidates[candidate_count++] = declared_offset - delta;
        }
        if (declared_offset <= p->len && delta <= p->len - declared_offset) {
            candidates[candidate_count++] = declared_offset + delta;
        }

        for (size_t i = 0; i < candidate_count; i++) {
            size_t candidate = candidates[i];
            if (candidate >= p->len ||
                !looks_like_indirect_obj_at(p->data, p->len, candidate)) {
                continue;
            }

            TspdfError parse_error = TSPDF_OK;
            TspdfObj *obj = parse_expected_indirect_obj_at(p, candidate,
                                                           expected_num, expected_gen,
                                                           &parse_error);
            if (obj) {
                if (out_repaired_offset) *out_repaired_offset = candidate;
                if (out_error) *out_error = TSPDF_OK;
                return obj;
            }
            if (parse_error == TSPDF_ERR_ALLOC) {
                if (out_error) *out_error = TSPDF_ERR_ALLOC;
                return NULL;
            }
        }
    }

    if (out_error) *out_error = TSPDF_ERR_PARSE;
    return NULL;
}

static TspdfObj resolving_obj_sentinel;

TspdfObj *tspdf_xref_resolve(TspdfReaderXref *xref, TspdfParser *p, uint32_t obj_num, TspdfObj **cache, TspdfCrypt *crypt) {
    if (obj_num >= xref->count) return NULL;
    if (!xref->entries[obj_num].in_use) return NULL;

    // Check cache
    if (cache[obj_num]) {
        if (cache[obj_num] == &resolving_obj_sentinel) {
            return NULL;
        }
        return cache[obj_num];
    }
    cache[obj_num] = &resolving_obj_sentinel;

    TspdfReaderXrefEntry *entry = &xref->entries[obj_num];

    if (!entry->compressed) {
        TspdfError parse_error = TSPDF_OK;
        TspdfObj *obj = parse_expected_indirect_obj_at(p, entry->offset,
                                                       obj_num, entry->gen,
                                                       &parse_error);
        if (!obj && parse_error != TSPDF_ERR_ALLOC) {
            size_t repaired_offset = entry->offset;
            obj = parse_expected_indirect_obj_near(p, entry->offset,
                                                   obj_num, entry->gen,
                                                   &repaired_offset, &parse_error);
            if (obj) {
                entry->offset = repaired_offset;
            }
        }
        if (!obj) goto fail;

        resolve_indirect_stream_length(xref, p, obj_num, cache, crypt, obj);

        // Decrypt strings if encryption is active
        if (crypt && !is_encrypt_dict_obj(xref, obj_num)) {
            // Skip /Type /XRef objects
            bool is_xref = false;
            TspdfObj *check_dict = (obj->type == TSPDF_OBJ_STREAM) ? obj->stream.dict :
                                  (obj->type == TSPDF_OBJ_DICT) ? obj : NULL;
            if (check_dict) {
                TspdfObj *type_val = tspdf_dict_get(check_dict, "Type");
                if (type_val && type_val->type == TSPDF_OBJ_NAME &&
                    strcmp((const char *)type_val->string.data, "XRef") == 0) {
                    is_xref = true;
                }
            }
            if (!is_xref) {
                decrypt_obj_strings(crypt, obj, obj_num, entry->gen);
            }
        }

        cache[obj_num] = obj;
        return obj;
    } else {
        // Compressed object inside an object stream
        // First resolve the object stream itself
        TspdfObj *stream_obj = tspdf_xref_resolve(xref, p, entry->stream_obj, cache, crypt);
        if (!stream_obj || stream_obj->type != TSPDF_OBJ_STREAM) goto fail;

        // Decompress the stream if not already done
        if (!stream_obj->stream.data) {
            const uint8_t *raw = p->data + stream_obj->stream.raw_offset;
            size_t raw_len = stream_obj->stream.raw_len;

            // For encrypted PDFs, decrypt the raw stream data before decompression
            uint8_t *decrypted_raw = NULL;
            size_t decrypted_len = raw_len;
            if (crypt) {
                TspdfReaderXrefEntry *stream_entry = &xref->entries[entry->stream_obj];
                decrypted_raw = tspdf_crypt_decrypt_stream(crypt, entry->stream_obj,
                                                          stream_entry->gen, raw, raw_len,
                                                          &decrypted_len);
                if (!decrypted_raw) goto fail;
                raw = decrypted_raw;
                raw_len = decrypted_len;
            }

            TspdfError decode_err = decode_stream_data(stream_obj->stream.dict, raw, raw_len,
                                                       &stream_obj->stream.data,
                                                       &stream_obj->stream.len);
            free(decrypted_raw);
            if (decode_err != TSPDF_OK) {
                stream_obj->stream.data = NULL;
                stream_obj->stream.len = 0;
                goto fail;
            }
        }

        // Get /First and /N from the object stream dict
        TspdfObj *first_obj = tspdf_dict_get(stream_obj->stream.dict, "First");
        TspdfObj *n_obj = tspdf_dict_get(stream_obj->stream.dict, "N");
        if (!first_obj || first_obj->type != TSPDF_OBJ_INT) goto fail;
        if (!n_obj || n_obj->type != TSPDF_OBJ_INT) goto fail;
        if (first_obj->integer < 0 || n_obj->integer < 0) goto fail;

        size_t first_offset = (size_t)first_obj->integer;
        size_t n = (size_t)n_obj->integer;

        if (entry->stream_idx >= n) goto fail;
        if (first_offset > stream_obj->stream.len) goto fail;

        // Parse the offset table from the /First-bounded header.
        // Format: pairs of (obj_num offset) repeated N times.
        TspdfParser header_parser;
        tspdf_parser_init(&header_parser, stream_obj->stream.data, first_offset, p->arena);

        // Read offset table entries
        size_t target_offset = 0;
        for (size_t i = 0; i <= (size_t)entry->stream_idx; i++) {
            tspdf_skip_whitespace(&header_parser);
            // Parse object number (skip it)
            TspdfObj *on = tspdf_parse_obj(&header_parser);
            if (!on || on->type != TSPDF_OBJ_INT) goto fail;
            if (on->integer < 0) goto fail;

            tspdf_skip_whitespace(&header_parser);
            // Parse relative offset
            TspdfObj *off = tspdf_parse_obj(&header_parser);
            if (!off || off->type != TSPDF_OBJ_INT) goto fail;
            if (off->integer < 0) goto fail;

            if (i == (size_t)entry->stream_idx) {
                if ((uint64_t)on->integer != obj_num) goto fail;
                target_offset = (size_t)off->integer;
            }
        }

        // Seek to the object data (first_offset + relative offset)
        if (target_offset > stream_obj->stream.len - first_offset) goto fail;
        TspdfParser object_parser;
        tspdf_parser_init(&object_parser, stream_obj->stream.data, stream_obj->stream.len, p->arena);
        object_parser.pos = first_offset + target_offset;
        if (object_parser.pos >= object_parser.len) goto fail;
        TspdfObj *result = tspdf_parse_obj(&object_parser);
        if (!result) goto fail;

        cache[obj_num] = result;
        return result;
    }

fail:
    cache[obj_num] = NULL;
    return NULL;
}
