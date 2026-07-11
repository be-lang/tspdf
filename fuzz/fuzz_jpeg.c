// libFuzzer harness for tspdf_jpeg_decode (baseline JPEG decoder).
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../src/image/jpeg_codec.h"
#include "../src/util/arena.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    TspdfArena arena = tspdf_arena_create(64 * 1024);
    TspdfRawImage img = {0};
    if (tspdf_jpeg_decode(data, size, &arena, &img)) {
        // Touch decoded pixels so the read is not elided.
        if (img.pixels && img.width > 0 && img.height > 0 && img.components > 0) {
            size_t n = (size_t)img.width * (size_t)img.height * (size_t)img.components;
            volatile uint8_t sink = 0;
            sink ^= img.pixels[0];
            sink ^= img.pixels[n - 1];
            (void)sink;
            // Re-encode the decoded image to exercise the encoder path too.
            uint8_t *out = NULL; size_t out_len = 0;
            (void)tspdf_jpeg_encode(&img, 75, &arena, &out, &out_len);
        }
    }
    tspdf_arena_destroy(&arena);
    return 0;
}
