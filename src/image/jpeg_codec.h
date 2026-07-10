#ifndef TSPDF_JPEG_CODEC_H
#define TSPDF_JPEG_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Baseline JPEG codec (from scratch, zero-dep) for lossy PDF image
 * recompression. Decoder: baseline sequential DCT, Huffman (SOF0),
 * grayscale and YCbCr 3-component with 4:4:4 / 4:2:2 / 4:2:0 sampling.
 * Progressive (SOF2), arithmetic, and CMYK JPEGs are rejected — callers
 * pass the original stream through untouched. Encoder: SOF0, standard
 * Annex K tables scaled by quality, 4:2:0 subsampling for RGB, 4:4:4
 * for grayscale. */

typedef struct {
    int width;
    int height;
    int components;  /* 1 = grayscale, 3 = RGB (interleaved) */
    uint8_t *pixels; /* width * height * components bytes, arena-allocated */
} TspdfRawImage;

/* Decode a baseline JPEG into interleaved 8-bit pixels (gray or RGB).
 * Returns false for unsupported or malformed input; *img is untouched
 * then. All allocations come from the arena. */
bool tspdf_jpeg_decode(const uint8_t *data, size_t len, TspdfArena *arena,
                       TspdfRawImage *img);

/* Encode gray (components=1) or RGB (components=3) pixels as a baseline
 * JPEG. quality is 1..100 (libjpeg-style scaling of the Annex K tables).
 * Returns false only on invalid arguments or allocation failure. Output
 * is arena-allocated. */
bool tspdf_jpeg_encode(const TspdfRawImage *img, int quality,
                       TspdfArena *arena, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
