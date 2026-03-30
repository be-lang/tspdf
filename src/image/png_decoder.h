#ifndef TSPDF_PNG_DECODER_H
#define TSPDF_PNG_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

// Free decoded image data
void png_image_free(PngImage *img);

#endif
