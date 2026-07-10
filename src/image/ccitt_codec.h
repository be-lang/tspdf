#ifndef TSPDF_CCITT_CODEC_H
#define TSPDF_CCITT_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CCITT Group 3/4 fax codec (from scratch, zero-dep) for bilevel PDF image
 * recompression. Decoder: /CCITTFaxDecode streams — G4/MMR (K < 0, the
 * common case), G3 one-dimensional MH (K = 0), and G3 mixed two-dimensional
 * (K > 0). /Columns, /Rows, /BlackIs1, /EncodedByteAlign, /EndOfLine and
 * /EndOfBlock are honored; T.4 "uncompressed mode" extensions are rejected.
 * Encoder: G4 (MMR) only — T.6 pass/vertical/horizontal modes with T.4 MH
 * terminating + makeup run codes for horizontal mode, closed by EOFB.
 *
 * Pixel format: one byte per pixel, PDF DeviceGray semantics after the
 * default /Decode — 0 = black, 255 = white. (With black_is_1 the decoder
 * emits 1-bits for black, which /DeviceGray maps to white, so the output
 * bytes are inverted exactly as a PDF viewer would show them.) The encoder
 * takes the same format (any byte < 128 counts as black) and emits a stream
 * for the default /BlackIs1 false convention. */

typedef struct {
    int k;                    /* < 0: G4, 0: G3 1-D, > 0: G3 mixed 2-D */
    int columns;              /* pixels per row; PDF default 1728 */
    int rows;                 /* expected rows; 0 = unknown (decode to EOFB/EOD) */
    bool black_is_1;          /* PDF default false */
    bool encoded_byte_align;  /* each coded line starts on a byte boundary */
    bool end_of_line;         /* EOL codes expected (decoder tolerates both) */
    bool end_of_block;        /* PDF default true: EOFB/RTC terminates data */
} TspdfCcittParams;

typedef struct {
    int width;
    int height;
    uint8_t *pixels;  /* width * height bytes, 0/255, arena-allocated */
} TspdfCcittBitmap;

/* Sensible defaults matching an empty /DecodeParms dict. */
void tspdf_ccitt_params_default(TspdfCcittParams *p);

/* Decode a CCITT fax stream. Returns false for malformed or unsupported
 * input (bounded loops, no out-of-bounds reads); *out is untouched then.
 * Pixel output comes from the arena. */
bool tspdf_ccitt_decode(const uint8_t *data, size_t len,
                        const TspdfCcittParams *params, TspdfArena *arena,
                        TspdfCcittBitmap *out);

/* Encode a bilevel bitmap (one byte per pixel, < 128 = black) as a G4/MMR
 * stream for /CCITTFaxDecode with /K -1 /Columns width /Rows height and
 * default /BlackIs1. Returns false only on invalid arguments or allocation
 * failure. Output is arena-allocated. */
bool tspdf_ccitt_encode_g4(const uint8_t *pixels, int width, int height,
                           TspdfArena *arena, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
