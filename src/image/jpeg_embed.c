#include "jpeg_embed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Read a 2-byte big-endian unsigned integer
static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

bool jpeg_load(JpegImage *img, const char *path) {
    memset(img, 0, sizeof(*img));

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 4) {
        fclose(f);
        return false;
    }

    // Read entire file
    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return false;
    }

    size_t nread = fread(data, 1, (size_t)fsize, f);
    fclose(f);

    if ((long)nread != fsize) {
        free(data);
        return false;
    }

    // Verify JPEG SOI marker
    if (data[0] != 0xFF || data[1] != 0xD8) {
        free(data);
        return false;
    }

    // Scan for SOF0 (0xFFC0) or SOF2 (0xFFC2) marker to extract dimensions
    bool found = false;
    size_t pos = 2;

    while (pos + 1 < (size_t)fsize) {
        // Find next marker
        if (data[pos] != 0xFF) {
            pos++;
            continue;
        }

        // Skip padding 0xFF bytes
        while (pos + 1 < (size_t)fsize && data[pos + 1] == 0xFF) {
            pos++;
        }

        if (pos + 1 >= (size_t)fsize) break;

        uint8_t marker = data[pos + 1];
        pos += 2;

        // SOF0 or SOF2 (baseline or progressive)
        if (marker == 0xC0 || marker == 0xC2) {
            // Need at least 8 bytes after marker: 2 (length) + 1 (precision) + 2 (height) + 2 (width) + 1 (components)
            if (pos + 8 > (size_t)fsize) break;

            // Skip length (2 bytes), precision (1 byte)
            img->height = read_be16(&data[pos + 3]);
            img->width  = read_be16(&data[pos + 5]);
            img->components = data[pos + 7];
            found = true;
            break;
        }

        // SOS marker (0xDA) - start of scan, stop searching
        if (marker == 0xDA) break;

        // For markers without a length field, skip
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            continue;
        }

        // Other markers: read 2-byte length and skip
        if (pos + 2 > (size_t)fsize) break;
        uint16_t seg_len = read_be16(&data[pos]);
        if (seg_len < 2 || pos + seg_len > (size_t)fsize) break;
        pos += seg_len;
    }

    if (!found) {
        free(data);
        return false;
    }

    if (img->width <= 0 || img->height <= 0) {
        free(data);
        return false;
    }

    img->data = data;
    img->data_len = (size_t)fsize;
    return true;
}

void jpeg_free(JpegImage *img) {
    free(img->data);
    img->data = NULL;
    img->data_len = 0;
}
