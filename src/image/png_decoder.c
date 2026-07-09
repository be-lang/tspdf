#include "png_decoder.h"
#include "../compress/deflate.h"
#include <limits.h>
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
    if (!path || !img) return false;
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

    // Decode the in-memory bytes; png_image_load_mem only reads file_data, so we
    // remain responsible for freeing the buffer we allocated here.
    bool ok = png_image_load_mem(file_data, (size_t)file_size, img);
    free(file_data);
    return ok;
}

bool png_image_load_mem(const uint8_t *file_data, size_t data_len, PngImage *img) {
    if (!file_data || !img) return false;
    memset(img, 0, sizeof(*img));

    // The signature (8) + IHDR length/tag/data/CRC (4+4+13+4) is the minimum a
    // valid PNG can be; bail before any read past the buffer.
    if (data_len < 33) return false;
    size_t file_size = data_len;

    // Check PNG signature
    if (memcmp(file_data, PNG_SIG, 8) != 0) {
        return false;
    }

    // Parse IHDR
    size_t pos = 8;
    uint32_t ihdr_len = read_u32_be(file_data + pos);
    if (memcmp(file_data + pos + 4, "IHDR", 4) != 0 || ihdr_len != 13) {
        return false;
    }
    const uint8_t *ihdr = file_data + pos + 8;
    uint32_t width_u = read_u32_be(ihdr);
    uint32_t height_u = read_u32_be(ihdr + 4);
    if (width_u == 0u || height_u == 0u) {
        return false;
    }
    if (width_u > (uint32_t)INT_MAX || height_u > (uint32_t)INT_MAX) {
        return false;
    }
    // Reject absurd dimensions up front. INT_MAX-sized axes pass the SIZE_MAX
    // multiply guards below on a 64-bit host yet would still demand gigabytes of
    // RAM, so a sane cap is the real defence against a tiny malformed IHDR
    // triggering a huge allocation. See TSPDF_PNG_MAX_DIMENSION in the header.
    if (width_u > TSPDF_PNG_MAX_DIMENSION || height_u > TSPDF_PNG_MAX_DIMENSION) {
        return false;
    }
    img->width = (int)width_u;
    img->height = (int)height_u;
    img->bit_depth = ihdr[8];
    int bit_depth = img->bit_depth;
    int color_type = ihdr[9];
    int compression = ihdr[10];
    int filter_method = ihdr[11];
    int interlace = ihdr[12];

    if (interlace != 0) {
        // Adam7 would need a full de-interlacing pass we do not implement.
        fprintf(stderr, "tspdf: interlaced PNG not supported\n");
        return false;
    }
    if (compression != 0 || filter_method != 0) {
        // 0 is the only compression/filter method the spec defines
        return false;
    }

    // Determine bytes per pixel (the filter delta; min 1 byte for palette)
    int bpp;
    switch (color_type) {
        case 0: bpp = 1; img->channels = 1; break;  // grayscale
        case 2: bpp = 3; img->channels = 3; break;  // RGB
        case 3: bpp = 1; img->channels = 1; break;  // palette indices
        case 4: bpp = 2; img->channels = 2; break;  // grayscale + alpha
        case 6: bpp = 4; img->channels = 4; break;  // RGBA
        default:
            return false;
    }

    // Palette images pack 1/2/4/8-bit indices; everything else is 8-bit only
    if (color_type == 3) {
        if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8) {
            return false;
        }
    } else if (bit_depth != 8) {
        return false;
    }

    // Collect all IDAT chunks (only count bytes from chunks that fully fit in
    // the file), and locate PLTE / tRNS for palette expansion on the same pass.
    size_t idat_total = 0;
    const uint8_t *plte = NULL;
    size_t plte_len = 0;
    const uint8_t *trns = NULL;
    size_t trns_len = 0;
    pos = 8;
    while (pos + 12 <= file_size) {
        uint32_t chunk_len = read_u32_be(file_data + pos);
        if (chunk_len > file_size - pos - 12) break;
        if (memcmp(file_data + pos + 4, "IDAT", 4) == 0) {
            size_t add = (size_t)chunk_len;
            if (idat_total > SIZE_MAX - add) {
                return false;
            }
            idat_total += add;
        } else if (memcmp(file_data + pos + 4, "PLTE", 4) == 0 && !plte) {
            plte = file_data + pos + 8;
            plte_len = chunk_len;
        } else if (memcmp(file_data + pos + 4, "tRNS", 4) == 0 && !trns) {
            trns = file_data + pos + 8;
            trns_len = chunk_len;
        }
        pos += 12 + chunk_len;
    }

    // Validate the palette: PLTE is mandatory for color type 3, holds up to 256
    // RGB triplets, and a tRNS alpha table may cover at most every entry.
    int plte_count = 0;
    if (color_type == 3) {
        if (!plte || plte_len == 0 || plte_len % 3 != 0 || plte_len > 256 * 3) {
            return false;
        }
        plte_count = (int)(plte_len / 3);
        if (trns && trns_len > (size_t)plte_count) {
            return false;
        }
    }

    uint8_t *idat_data = (uint8_t *)malloc(idat_total);
    if (!idat_data && idat_total > 0) { return false; }

    size_t idat_pos = 0;
    pos = 8;
    while (pos + 12 <= file_size) {
        uint32_t chunk_len = read_u32_be(file_data + pos);
        if (chunk_len > file_size - pos - 12) break;
        if (memcmp(file_data + pos + 4, "IDAT", 4) == 0) {
            memcpy(idat_data + idat_pos, file_data + pos + 8, chunk_len);
            idat_pos += chunk_len;
        }
        pos += 12 + chunk_len;
    }

    if (idat_pos != idat_total) { free(idat_data); return false; }

    // Decompress IDAT data (zlib/deflate)
    size_t raw_len = 0;
    uint8_t *raw = deflate_decompress(idat_data, idat_total, &raw_len);
    free(idat_data);

    if (!raw) return false;

    // Expected raw size: (1 + stride) * height (1 filter byte per row).
    // Palette rows pack width*bit_depth bits (rounded up to whole bytes);
    // the other color types are 8-bit so their stride is width*bpp bytes.
    size_t bpp_sz = (size_t)bpp;
    size_t stride;
    if (color_type == 3) {
        // width <= TSPDF_PNG_MAX_DIMENSION and bit_depth <= 8, so this cannot
        // overflow, but keep the explicit guard style of the branch below.
        if ((size_t)img->width > (SIZE_MAX - 7u) / (size_t)bit_depth) {
            free(raw);
            return false;
        }
        stride = ((size_t)img->width * (size_t)bit_depth + 7u) / 8u;
    } else {
        if ((uint32_t)img->width > SIZE_MAX / bpp_sz) {
            free(raw);
            return false;
        }
        stride = (size_t)img->width * bpp_sz;
    }
    if ((size_t)img->height > SIZE_MAX / (stride + 1u)) {
        free(raw);
        return false;
    }
    size_t expected = (stride + 1u) * (size_t)img->height;
    if (raw_len != expected) {
        free(raw);
        return false;
    }

    // Allocate output pixels (always RGB or RGBA, 3 or 4 channels).
    // A palette image becomes RGBA only when a tRNS alpha table is present.
    int out_channels = (color_type == 4 || color_type == 6 ||
                        (color_type == 3 && trns)) ? 4 : 3;
    img->channels = out_channels;
    size_t oc = (size_t)out_channels;
    size_t w = (size_t)img->width;
    size_t h = (size_t)img->height;
    if (h > 0 && w > SIZE_MAX / h) {
        free(raw);
        return false;
    }
    size_t npx = w * h;
    if (oc > 0 && npx > SIZE_MAX / oc) {
        free(raw);
        return false;
    }
    size_t pix_bytes = npx * oc;
    img->pixels = (uint8_t *)malloc(pix_bytes);
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

        // Convert to output format. Use size_t for the row-offset arithmetic so
        // the product cannot overflow int, even though pix_bytes above already
        // bounds it; this keeps the index math well-defined for any valid image.
        uint8_t *out = img->pixels + (size_t)y * (size_t)img->width * (size_t)out_channels;
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
                case 3: {  // palette index → RGB(A)
                    // Sub-byte depths pack indices MSB-first within each byte.
                    unsigned idx;
                    if (bit_depth == 8) {
                        idx = cur_row[x];
                    } else {
                        size_t bit = (size_t)x * (size_t)bit_depth;
                        unsigned shift = 8u - (unsigned)bit_depth - (unsigned)(bit % 8u);
                        idx = (cur_row[bit / 8u] >> shift) & ((1u << bit_depth) - 1u);
                    }
                    if ((int)idx >= plte_count) {
                        // Index past the PLTE entry count: malformed image
                        free(raw); free(prev_row); free(cur_row); free(img->pixels);
                        img->pixels = NULL;
                        return false;
                    }
                    out[x * out_channels + 0] = plte[idx * 3 + 0];
                    out[x * out_channels + 1] = plte[idx * 3 + 1];
                    out[x * out_channels + 2] = plte[idx * 3 + 2];
                    if (out_channels == 4) {
                        // tRNS covers the first trns_len entries; the rest are opaque
                        out[x * 4 + 3] = ((size_t)idx < trns_len) ? trns[idx] : 255;
                    }
                    break;
                }
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

// --- IDAT passthrough -------------------------------------------------------

// Row stride in bytes for the filtered raster (excluding the filter byte).
static size_t png_row_stride(int width, int bit_depth, int color_type) {
    size_t bits_per_px = (size_t)bit_depth * (color_type == 2 ? 3u : 1u);
    return ((size_t)width * bits_per_px + 7u) / 8u;
}

bool png_read_passthrough(const char *path, PngPassthrough *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long file_size_l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size_l < 33) { fclose(f); return false; }
    size_t file_size = (size_t)file_size_l;

    uint8_t *data = (uint8_t *)malloc(file_size);
    if (!data) { fclose(f); return false; }
    if (fread(data, 1, file_size, f) != file_size) {
        free(data);
        fclose(f);
        return false;
    }
    fclose(f);

    bool ok = false;
    uint8_t *raw = NULL;

    if (memcmp(data, PNG_SIG, 8) != 0) goto done;

    // IHDR — same validation as the decode path (see png_image_load_mem)
    size_t pos = 8;
    if (read_u32_be(data + pos) != 13 ||
        memcmp(data + pos + 4, "IHDR", 4) != 0) goto done;
    const uint8_t *ihdr = data + pos + 8;
    uint32_t width_u = read_u32_be(ihdr);
    uint32_t height_u = read_u32_be(ihdr + 4);
    if (width_u == 0u || height_u == 0u ||
        width_u > TSPDF_PNG_MAX_DIMENSION || height_u > TSPDF_PNG_MAX_DIMENSION) {
        goto done;
    }
    int bit_depth = ihdr[8];
    int color_type = ihdr[9];
    if (ihdr[10] != 0 || ihdr[11] != 0) goto done;  // compression/filter method
    if (ihdr[12] != 0) goto done;                    // interlaced: row order differs
    // Only gray/RGB/palette can passthrough — types 4/6 interleave alpha with
    // color, which PDF image streams cannot represent.
    if (color_type != 0 && color_type != 2 && color_type != 3) goto done;
    if (color_type == 3) {
        if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8) goto done;
    } else if (bit_depth != 8) {
        goto done;  // sub-8/16-bit gray/RGB: keep the decode fallback behavior
    }
    out->width = (int)width_u;
    out->height = (int)height_u;
    out->bit_depth = bit_depth;
    out->color_type = color_type;

    // Chunk scan: total IDAT size plus PLTE/tRNS, mirroring the decode path.
    size_t idat_total = 0;
    const uint8_t *plte = NULL;
    size_t plte_len = 0;
    bool have_trns = false;
    pos = 8;
    while (pos + 12 <= file_size) {
        uint32_t chunk_len = read_u32_be(data + pos);
        if (chunk_len > file_size - pos - 12) break;
        if (memcmp(data + pos + 4, "IDAT", 4) == 0) {
            if (idat_total > SIZE_MAX - chunk_len) goto done;
            idat_total += chunk_len;
        } else if (memcmp(data + pos + 4, "PLTE", 4) == 0 && !plte) {
            plte = data + pos + 8;
            plte_len = chunk_len;
        } else if (memcmp(data + pos + 4, "tRNS", 4) == 0) {
            have_trns = true;
        }
        pos += 12 + chunk_len;
    }
    if (idat_total == 0) goto done;

    if (color_type == 3) {
        if (!plte || plte_len == 0 || plte_len % 3 != 0 || plte_len > 256 * 3) goto done;
        out->palette_count = (int)(plte_len / 3);
        memcpy(out->palette, plte, plte_len);
        out->has_alpha = have_trns;
    }
    // tRNS on gray/RGB (color-key transparency) is ignored, matching the
    // decode path, so passthrough loses nothing relative to it.

    out->idat = (uint8_t *)malloc(idat_total);
    if (!out->idat) goto done;
    pos = 8;
    while (pos + 12 <= file_size) {
        uint32_t chunk_len = read_u32_be(data + pos);
        if (chunk_len > file_size - pos - 12) break;
        if (memcmp(data + pos + 4, "IDAT", 4) == 0) {
            memcpy(out->idat + out->idat_len, data + pos + 8, chunk_len);
            out->idat_len += chunk_len;
        }
        pos += 12 + chunk_len;
    }
    if (out->idat_len != idat_total) goto done;

    // Validate before vouching for the bytes: the stream must inflate to
    // exactly (stride + 1) * height with every row filter byte in 0..4.
    // Anything else would embed a stream PDF readers cannot decode.
    size_t stride = png_row_stride(out->width, bit_depth, color_type);
    if ((size_t)out->height > SIZE_MAX / (stride + 1u)) goto done;
    size_t expected = (stride + 1u) * (size_t)out->height;
    size_t raw_len = 0;
    raw = deflate_decompress(out->idat, out->idat_len, &raw_len);
    if (!raw || raw_len != expected) goto done;
    for (int y = 0; y < out->height; y++) {
        if (raw[(size_t)y * (stride + 1u)] > 4) goto done;
    }

    ok = true;
done:
    free(raw);
    free(data);
    if (!ok) png_passthrough_free(out);
    return ok;
}

void png_passthrough_free(PngPassthrough *pt) {
    if (!pt) return;
    free(pt->idat);
    memset(pt, 0, sizeof(*pt));
}
