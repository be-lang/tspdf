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

// IDAT passthrough: for a non-interlaced gray/RGB/palette PNG the raw zlib
// stream inside the IDAT chunks is already a valid PDF FlateDecode stream when
// paired with /DecodeParms << /Predictor 15 /Colors N /BitsPerComponent B
// /Columns W >> (PNG row filters are exactly PDF's PNG predictors). This
// struct carries everything the writer needs to embed those bytes verbatim —
// zero recompression, zero size loss.
typedef struct {
    int width;
    int height;
    int bit_depth;              // 8 for gray/RGB; 1/2/4/8 for palette
    int color_type;             // 0 = gray, 2 = RGB, 3 = palette
    uint8_t *idat;              // concatenated IDAT bytes (malloc'd)
    size_t idat_len;
    uint8_t palette[256 * 3];   // RGB triplets (color_type 3 only)
    int palette_count;          // PLTE entries; 0 unless color_type 3
    bool has_alpha;             // palette tRNS present: caller must build a
                                // decoded soft mask via png_image_load()
} PngPassthrough;

// Extract passthrough info from a PNG file. Returns false when the image is
// not eligible (interlaced, alpha color types 4/6, non-8-bit gray/RGB, or a
// damaged IDAT) — callers then fall back to png_image_load(). The IDAT is
// validated (inflates to exactly the expected raster size, every row filter
// byte is 0..4) so truncated or garbage streams are never embedded.
// On success the caller must free with png_passthrough_free().
bool png_read_passthrough(const char *path, PngPassthrough *out);
void png_passthrough_free(PngPassthrough *pt);

#endif
