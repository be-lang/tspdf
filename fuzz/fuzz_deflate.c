// libFuzzer harness for the from-scratch DEFLATE/zlib compressor round-trip.
//
// Build:  make fuzz
// Run:    ./build/fuzz/fuzz_deflate fuzz/corpus/deflate
//
// deflate_compress() encodes every stream tspdf writes, so a wrong bit pattern
// silently corrupts output PDFs. This harness compresses arbitrary input and
// requires the library's own inflate to reproduce it byte-identically; any
// mismatch, unexpected NULL, or decode failure aborts.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../src/compress/deflate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, size, &comp_len);
    if (!comp) return 0;  // allocation failure is the only legal reason here

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(comp, comp_len, &dec_len);
    if (!dec || dec_len != size || (size > 0 && memcmp(dec, data, size) != 0)) {
        __builtin_trap();  // round-trip broke: not a valid/faithful stream
    }

    free(comp);
    free(dec);
    return 0;
}
