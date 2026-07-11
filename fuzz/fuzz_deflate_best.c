// libFuzzer harness for deflate_compress_best round-trip (BEST search level).
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../src/compress/deflate.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress_best(data, size, &comp_len);
    if (!comp) return 0;

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(comp, comp_len, &dec_len);
    if (!dec || dec_len != size || (size > 0 && memcmp(dec, data, size) != 0)) {
        __builtin_trap();
    }
    free(comp);
    free(dec);
    return 0;
}
