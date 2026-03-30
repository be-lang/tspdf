#include "png_decoder.h"
#include "../compress/deflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PNG file format: signature + chunks (IHDR, IDAT*, IEND)
// IDAT contains zlib-compressed filtered scanlines

static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t PNG_SIG[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// PNG filter types
#define FILTER_NONE    0
#define FILTER_SUB     1
#define FILTER_UP      2
#define FILTER_AVERAGE 3
#define FILTER_PAETH   4

static int paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool png_image_load(const char *path, PngImage *img) {
    memset(img, 0, sizeof(*img));

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    // Read entire file
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 8) { fclose(f); return false; }

    uint8_t *file_data = (uint8_t *)malloc(file_size);
    if (!file_data) { fclose(f); return false; }
    if (fread(file_data, 1, file_size, f) != (size_t)file_size) {
        free(file_data);
        fclose(f);
        return false;
    }
    fclose(f);

    // Check PNG signature
    if (memcmp(file_data, PNG_SIG, 8) != 0) {
        free(file_data);
        return false;
    }

    // Parse IHDR
    size_t pos = 8;
    uint32_t ihdr_len = read_u32_be(file_data + pos);
    if (memcmp(file_data + pos + 4, "IHDR", 4) != 0 || ihdr_len != 13) {
        free(file_data);
        return false;
    }
    const uint8_t *ihdr = file_data + pos + 8;
    img->width = (int)read_u32_be(ihdr);
    img->height = (int)read_u32_be(ihdr + 4);
    img->bit_depth = ihdr[8];
    int color_type = ihdr[9];
    int compression = ihdr[10];
    int filter_method = ihdr[11];
    int interlace = ihdr[12];

    if (compression != 0 || filter_method != 0 || interlace != 0) {
        // Only support standard non-interlaced PNG
        free(file_data);
        return false;
    }

    // Determine bytes per pixel
    int bpp;
    switch (color_type) {
        case 0: bpp = 1; img->channels = 1; break;  // grayscale
        case 2: bpp = 3; img->channels = 3; break;  // RGB
        case 4: bpp = 2; img->channels = 2; break;  // grayscale + alpha
        case 6: bpp = 4; img->channels = 4; break;  // RGBA
        default:
            free(file_data);
            return false;  // indexed/palette not supported yet
    }

    // Only support 8-bit depth for now
    if (img->bit_depth != 8) {
        free(file_data);
        return false;
    }

    // Collect all IDAT chunks
    size_t idat_total = 0;
    pos = 8;
    while (pos + 12 <= (size_t)file_size) {
        uint32_t chunk_len = read_u32_be(file_data + pos);
        if (memcmp(file_data + pos + 4, "IDAT", 4) == 0) {
            idat_total += chunk_len;
        }
        if (chunk_len > (size_t)file_size - pos - 12) break;  // bounds check
        pos += 12 + chunk_len;
    }

    uint8_t *idat_data = (uint8_t *)malloc(idat_total);
    if (!idat_data) { free(file_data); return false; }

    size_t idat_pos = 0;
    pos = 8;
    while (pos + 12 <= (size_t)file_size) {
        uint32_t chunk_len = read_u32_be(file_data + pos);
        if (pos + 8 + chunk_len > (size_t)file_size) break;  // bounds check
        if (memcmp(file_data + pos + 4, "IDAT", 4) == 0) {
            memcpy(idat_data + idat_pos, file_data + pos + 8, chunk_len);
            idat_pos += chunk_len;
        }
        pos += 12 + chunk_len;
    }

    free(file_data);

    // Decompress IDAT data (zlib/deflate)
    size_t raw_len = 0;
    uint8_t *raw = deflate_decompress(idat_data, idat_total, &raw_len);
    free(idat_data);

    if (!raw) return false;

    // Expected raw size: (1 + width*bpp) * height (1 filter byte per row)
    size_t stride = (size_t)(img->width * bpp);
    size_t expected = (stride + 1) * (size_t)img->height;
    if (raw_len != expected) {
        free(raw);
        return false;
    }

    // Allocate output pixels (always RGB or RGBA, 3 or 4 channels)
    int out_channels = (color_type == 4 || color_type == 6) ? 4 : 3;
    img->channels = out_channels;
    img->pixels = (uint8_t *)malloc(img->width * img->height * out_channels);
    if (!img->pixels) { free(raw); return false; }

    // Unfilter scanlines
    uint8_t *prev_row = (uint8_t *)calloc(stride, 1);
    uint8_t *cur_row = (uint8_t *)malloc(stride);
    if (!prev_row || !cur_row) {
        free(prev_row); free(cur_row); free(raw); free(img->pixels);
        img->pixels = NULL;
        return false;
    }

    for (int y = 0; y < img->height; y++) {
        size_t row_offset = (size_t)y * (stride + 1);
        uint8_t filter_type = raw[row_offset];
        const uint8_t *src = raw + row_offset + 1;
        memcpy(cur_row, src, stride);

        // Apply inverse filter
        for (size_t x = 0; x < stride; x++) {
            uint8_t a = (x >= (size_t)bpp) ? cur_row[x - bpp] : 0;
            uint8_t b = prev_row[x];
            uint8_t c = (x >= (size_t)bpp) ? prev_row[x - bpp] : 0;

            switch (filter_type) {
                case FILTER_NONE:    break;
                case FILTER_SUB:     cur_row[x] += a; break;
                case FILTER_UP:      cur_row[x] += b; break;
                case FILTER_AVERAGE: cur_row[x] += (uint8_t)((a + b) / 2); break;
                case FILTER_PAETH:   cur_row[x] += (uint8_t)paeth_predictor(a, b, c); break;
                default: free(raw); free(prev_row); free(cur_row); free(img->pixels);
                         img->pixels = NULL; return false;
            }
        }

        // Convert to output format
        uint8_t *out = img->pixels + y * img->width * out_channels;
        for (int x = 0; x < img->width; x++) {
            switch (color_type) {
                case 0:  // grayscale → RGB
                    out[x * 3 + 0] = cur_row[x];
                    out[x * 3 + 1] = cur_row[x];
                    out[x * 3 + 2] = cur_row[x];
                    break;
                case 2:  // RGB → RGB
                    out[x * 3 + 0] = cur_row[x * 3 + 0];
                    out[x * 3 + 1] = cur_row[x * 3 + 1];
                    out[x * 3 + 2] = cur_row[x * 3 + 2];
                    break;
                case 4:  // gray+alpha → RGBA
                    out[x * 4 + 0] = cur_row[x * 2 + 0];
                    out[x * 4 + 1] = cur_row[x * 2 + 0];
                    out[x * 4 + 2] = cur_row[x * 2 + 0];
                    out[x * 4 + 3] = cur_row[x * 2 + 1];
                    break;
                case 6:  // RGBA → RGBA
                    out[x * 4 + 0] = cur_row[x * 4 + 0];
                    out[x * 4 + 1] = cur_row[x * 4 + 1];
                    out[x * 4 + 2] = cur_row[x * 4 + 2];
                    out[x * 4 + 3] = cur_row[x * 4 + 3];
                    break;
            }
        }

        memcpy(prev_row, cur_row, stride);
    }

    free(raw);
    free(prev_row);
    free(cur_row);
    img->bit_depth = 8;
    return true;
}

void png_image_free(PngImage *img) {
    free(img->pixels);
    img->pixels = NULL;
}
