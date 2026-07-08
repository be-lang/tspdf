// libFuzzer harness for the from-scratch DEFLATE/zlib decompressor.
//
// Build:  make fuzz
// Run:    ./build/fuzz/fuzz_inflate fuzz/corpus/inflate
//
// deflate_decompress() consumes attacker-controlled zlib streams (PDF
// FlateDecode, PNG IDAT). It is currently only exercised by its own round-trips,
// so this harness feeds it arbitrary bytes to shake out Huffman-table, distance,
// and output-buffer-growth bugs. The output cap (TSPDF_DEFLATE_MAX_OUTPUT) is
// honoured by the decompressor itself, so even a tiny "zip bomb" stays bounded.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../src/compress/deflate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t out_len = 0;
    uint8_t *out = deflate_decompress(data, size, &out_len);
    if (out) {
        // Read the first and last byte so any over-allocation vs. reported
        // length mismatch trips ASan.
        if (out_len > 0) {
            volatile uint8_t sink = out[0] ^ out[out_len - 1];
            (void)sink;
        }
        free(out);
    }
    return 0;
}
