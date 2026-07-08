#include "buffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

TspdfBuffer tspdf_buffer_create(size_t initial_capacity) {
    TspdfBuffer buf = {0};
    buf.data = (uint8_t *)malloc(initial_capacity);
    if (buf.data) {
        buf.capacity = initial_capacity;
    } else {
        buf.error = true;
    }
    return buf;
}

void tspdf_buffer_destroy(TspdfBuffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

static void buffer_grow(TspdfBuffer *buf, size_t needed) {
    // Guard against size_t wrap: an attacker-controlled `needed` (the inflate
    // path appends untrusted-length data here) must never make len+needed wrap
    // to a small value, which would skip the realloc and leave a short buffer
    // for the caller's memcpy. Mirror the SIZE_MAX-relative guards used in the
    // reader (tspr_xref.c ensure_decode_capacity).
    if (needed > SIZE_MAX - buf->len) {
        buf->error = true;
        return;
    }
    size_t required = buf->len + needed;
    if (required <= buf->capacity) return;

    size_t new_cap = buf->capacity;
    if (new_cap < 16) new_cap = 16;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;  // doubling would overflow; clamp to exact need
            break;
        }
        new_cap *= 2;
    }
    uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
    if (new_data) {
        buf->data = new_data;
        buf->capacity = new_cap;
    } else {
        buf->error = true;
    }
}

void tspdf_buffer_append(TspdfBuffer *buf, const void *data, size_t len) {
    buffer_grow(buf, len);
    // Overflow-safe room check: if grow failed (or refused on wrap) capacity is
    // unchanged, so len > capacity - len catches it without computing len+len.
    if (len > buf->capacity - buf->len) return;  // alloc failed
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void tspdf_buffer_append_str(TspdfBuffer *buf, const char *str) {
    tspdf_buffer_append(buf, str, strlen(str));
}

void tspdf_buffer_append_byte(TspdfBuffer *buf, uint8_t byte) {
    buffer_grow(buf, 1);
    if (buf->len >= buf->capacity) return;  // alloc failed
    buf->data[buf->len++] = byte;
}

void tspdf_buffer_printf(TspdfBuffer *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    // First pass: measure
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed > 0) {
        size_t want = (size_t)needed + 1;
        buffer_grow(buf, want);
        // Overflow-safe room check (see tspdf_buffer_append).
        if (want > buf->capacity - buf->len) { va_end(args); return; }
        vsnprintf((char *)(buf->data + buf->len), want, fmt, args);
        buf->len += (size_t)needed;
    }
    va_end(args);
}

void tspdf_buffer_append_int(TspdfBuffer *buf, int val) {
    char tmp[16];
    int len = 0;
    unsigned int uval;
    if (val < 0) {
        tmp[len++] = '-';
        uval = (unsigned int)(-(val + 1)) + 1u;
    } else {
        uval = (unsigned int)val;
    }
    if (uval == 0) {
        tmp[len++] = '0';
    } else {
        char digits[12];
        int dlen = 0;
        while (uval > 0) {
            digits[dlen++] = '0' + (char)(uval % 10);
            uval /= 10;
        }
        for (int i = dlen - 1; i >= 0; i--) {
            tmp[len++] = digits[i];
        }
    }
    tspdf_buffer_append(buf, tmp, (size_t)len);
}

void tspdf_buffer_append_int64(TspdfBuffer *buf, int64_t val) {
    char tmp[24];
    int len = 0;
    uint64_t uval;
    if (val < 0) {
        tmp[len++] = '-';
        uval = (uint64_t)(-(val + 1)) + 1u;
    } else {
        uval = (uint64_t)val;
    }
    if (uval == 0) {
        tmp[len++] = '0';
    } else {
        char digits[20];
        int dlen = 0;
        while (uval > 0) {
            digits[dlen++] = '0' + (char)(uval % 10);
            uval /= 10;
        }
        for (int i = dlen - 1; i >= 0; i--) {
            tmp[len++] = digits[i];
        }
    }
    tspdf_buffer_append(buf, tmp, (size_t)len);
}

void tspdf_buffer_append_double(TspdfBuffer *buf, double val, int decimal_places) {
    if (val < 0) {
        tspdf_buffer_append_byte(buf, '-');
        val = -val;
    }

    double mult = 1.0;
    for (int i = 0; i < decimal_places; i++) mult *= 10.0;
    unsigned long long scaled = (unsigned long long)(val * mult + 0.5);

    unsigned long long int_part = scaled / (unsigned long long)mult;
    unsigned long long frac_part = scaled % (unsigned long long)mult;

    tspdf_buffer_append_int64(buf, (int64_t)int_part);

    if (frac_part > 0) {
        tspdf_buffer_append_byte(buf, '.');
        char frac_str[16];
        int flen = decimal_places;
        unsigned long long tmp = frac_part;
        for (int i = decimal_places - 1; i >= 0; i--) {
            frac_str[i] = '0' + (char)(tmp % 10);
            tmp /= 10;
        }
        while (flen > 1 && frac_str[flen - 1] == '0') flen--;
        tspdf_buffer_append(buf, frac_str, (size_t)flen);
    }
}

void tspdf_buffer_reset(TspdfBuffer *buf) {
    buf->len = 0;
}
