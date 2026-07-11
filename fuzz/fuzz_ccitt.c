// libFuzzer harness for tspdf_ccitt_decode (CCITT G3/G4 fax decoder).
// The first bytes of the input parametrize the /CCITTFaxDecode params so a
// single corpus exercises K<0 (G4), K=0 (G3 1-D) and K>0 (G3 mixed) paths,
// plus BlackIs1 / EncodedByteAlign / EndOfLine / EndOfBlock toggles.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../src/image/ccitt_codec.h"
#include "../src/util/arena.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    TspdfArena arena = tspdf_arena_create(64 * 1024);

    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    uint8_t hdr = data[0];
    // k in {-1, 0, 2} from low bits
    switch (hdr & 3) { case 0: p.k = -1; break; case 1: p.k = 0; break; default: p.k = 2; }
    p.black_is_1        = (hdr >> 2) & 1;
    p.encoded_byte_align = (hdr >> 3) & 1;
    p.end_of_line       = (hdr >> 4) & 1;
    p.end_of_block      = (hdr >> 5) & 1;
    // columns 1..2048 from next two bytes; rows 0..255
    p.columns = 1 + (((int)data[1] << 3) | (data[2] & 7));
    if (p.columns > 4096) p.columns = 4096;
    p.rows = data[3];

    TspdfCcittBitmap out = {0};
    if (tspdf_ccitt_decode(data + 4, size - 4, &p, &arena, &out)) {
        if (out.pixels && out.width > 0 && out.height > 0) {
            size_t n = (size_t)out.width * (size_t)out.height;
            volatile uint8_t sink = 0;
            sink ^= out.pixels[0];
            sink ^= out.pixels[n - 1];
            (void)sink;
            // Round-trip through the G4 encoder.
            uint8_t *enc = NULL; size_t enc_len = 0;
            (void)tspdf_ccitt_encode_g4(out.pixels, out.width, out.height, &arena, &enc, &enc_len);
        }
    }
    tspdf_arena_destroy(&arena);
    return 0;
}
