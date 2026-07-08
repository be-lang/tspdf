// libFuzzer harness for the from-scratch PNG decoder.
//
// Build:  make fuzz
// Run:    ./build/fuzz/fuzz_png fuzz/corpus/png
//
// png_image_load_mem() parses an untrusted PNG: IHDR dimensions, IDAT chunk
// chaining, zlib inflation, and per-scanline unfiltering (Sub/Up/Average/Paeth)
// all run on attacker-controlled sizes. This harness feeds it arbitrary bytes to
// catch IDAT-length, stride, and unfilter buffer arithmetic bugs. The in-memory
// entry point added for this harness avoids any temp-file detour.

#include <stdint.h>
#include <stddef.h>

#include "../src/image/png_decoder.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    PngImage img;
    if (png_image_load_mem(data, size, &img)) {
        png_image_free(&img);
    }
    return 0;
}
