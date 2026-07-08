#ifndef TSPDF_PNG_DECODER_H
#define TSPDF_PNG_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Sane upper bound on either PNG dimension. ISO/IEC 15948 (and the libpng
// reference decoder) permit dimensions up to 2^31-1, but no legitimate image
// approaches that. Capping each axis here means the worst-case raster size is
// bounded well before the overflow-checked size_t math in png_decoder.c, so a
// tiny malformed IHDR cannot request a multi-gigabyte allocation (DoS) on a
// 64-bit host where the SIZE_MAX multiply checks alone would still pass.
#define TSPDF_PNG_MAX_DIMENSION 65535

// Decoded PNG image data
typedef struct {
    uint8_t *pixels;      // raw pixel data (RGB or RGBA, row-major)
    int width;
    int height;
    int channels;         // 3 = RGB, 4 = RGBA
    int bit_depth;        // always converted to 8
} PngImage;

// Decode a PNG file into raw pixel data.
// Returns true on success. Caller must free pixels with png_image_free().
bool png_image_load(const char *path, PngImage *img);

// Decode a PNG already resident in memory. The input buffer is NOT taken over
// (it is only read), so the caller still owns `data`. Returns true on success;
// the caller must free img->pixels with png_image_free(). This is the in-memory
// entry point used by fuzzers and any caller that already holds the bytes, so
// no file I/O detour is needed.
bool png_image_load_mem(const uint8_t *data, size_t len, PngImage *img);

// Free decoded image data
void png_image_free(PngImage *img);

#endif
