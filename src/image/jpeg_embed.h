#ifndef TSPDF_JPEG_EMBED_H
#define TSPDF_JPEG_EMBED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int width;
    int height;
    int components;  // 1=grayscale, 3=RGB
    uint8_t *data;   // raw JPEG file data (NOT decoded pixels)
    size_t data_len;
} JpegImage;

bool jpeg_load(JpegImage *img, const char *path);
void jpeg_free(JpegImage *img);

#endif
