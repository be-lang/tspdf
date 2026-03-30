#ifndef TSPDF_BUFFER_H
#define TSPDF_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// A growable byte buffer for building PDF output
typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
    bool error;       // set true if any allocation failed
} TspdfBuffer;

TspdfBuffer tspdf_buffer_create(size_t initial_capacity);
void tspdf_buffer_destroy(TspdfBuffer *buf);
void tspdf_buffer_append(TspdfBuffer *buf, const void *data, size_t len);
void tspdf_buffer_append_str(TspdfBuffer *buf, const char *str);
void tspdf_buffer_append_byte(TspdfBuffer *buf, uint8_t byte);
void tspdf_buffer_printf(TspdfBuffer *buf, const char *fmt, ...);
void tspdf_buffer_append_double(TspdfBuffer *buf, double val, int decimal_places);
void tspdf_buffer_append_int(TspdfBuffer *buf, int val);
void tspdf_buffer_append_int64(TspdfBuffer *buf, int64_t val);
void tspdf_buffer_reset(TspdfBuffer *buf);

#endif
