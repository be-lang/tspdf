// libFuzzer harness for the from-scratch TrueType/OpenType parser.
//
// Build:  make fuzz
// Run:    ./build/fuzz/fuzz_ttf fuzz/corpus/ttf
//
// ttf_load_from_memory() parses an untrusted sfnt table directory plus head/
// hhea/maxp/hmtx/cmap/name/os2/kern tables — all driven by 16- and 32-bit
// offsets read straight from the input. This harness feeds it arbitrary bytes to
// catch out-of-bounds table reads, bad glyph counts, and cmap subtable
// arithmetic errors.
//
// Ownership: ttf_load_from_memory() takes a malloc'd buffer. On success ttf_free
// frees it; on failure the caller still owns it (see ttf_parser.c). We mirror
// that contract exactly so LeakSanitizer stays quiet.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../src/font/ttf_parser.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;

    // ttf_load_from_memory takes ownership of a heap buffer, so hand it a private
    // copy rather than the fuzzer's read-only mapping.
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) return 0;
    memcpy(buf, data, size);

    TTF_Font font;
    if (ttf_load_from_memory(&font, buf, size)) {
        // Drive the lookup paths over the parsed (possibly degenerate) tables.
        for (uint32_t cp = 0x20; cp < 0x80; cp++) {
            uint16_t gid = ttf_get_glyph_index(&font, cp);
            (void)ttf_get_glyph_advance(&font, gid);
        }
        (void)ttf_measure_string(&font, "Fuzzing 123");
        ttf_free(&font);  // frees buf
    } else {
        // On failure ttf_load_from_memory does NOT free the buffer.
        free(buf);
    }
    return 0;
}
