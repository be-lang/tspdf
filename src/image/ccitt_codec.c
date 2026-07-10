// CCITT Group 3/4 fax codec (ITU-T T.4 / T.6), written from scratch for
// bilevel PDF image recompression. See ccitt_codec.h for the API contract.
//
// Decoder notes:
//  - Lines are decoded into "changing element" position arrays (the classic
//    T.4 representation): positions where the color flips, strictly
//    increasing, starting from an all-white assumption. Rendering a line is
//    then a couple of memsets per black span.
//  - Run-length codes are decoded through flat lookup tables (12-bit for
//    white, 13-bit for black — the maximum T.4 code lengths) built once from
//    the canonical code lists below, so decode cost is per code word, not
//    per table entry.
//  - Everything is bounded: columns/rows caps, per-line iteration caps, a
//    total-pixel cap, and a bit reader that reports exhaustion instead of
//    reading past the buffer. Malformed input returns false, never crashes.
//  - K > 0 (mixed 2-D) lines are tagged 1-D/2-D by the bit that follows each
//    EOL. When a K > 0 stream carries no EOLs (EndOfLine false) the tag bit
//    still precedes each line's data, which matches what real encoders
//    (e.g. Ghostscript's CCITTFaxEncode) emit.
//
// Encoder notes (G4 only):
//  - Standard T.6 vertical/pass/horizontal coding against the previous line,
//    with T.4 MH terminating + makeup codes (extended makeup codes above
//    1728, repeated 2560-makeups for very long runs) for horizontal mode.
//  - Output ends with EOFB (two EOLs) and is zero-padded to a byte.

#include "ccitt_codec.h"

#include <stdlib.h>
#include <string.h>

// --- Safety caps ---

#define CCITT_MAX_COLUMNS (1 << 20)
#define CCITT_MAX_ROWS (1 << 20)
#define CCITT_MAX_PIXELS ((size_t)1 << 28)  // 256 Mpx decoded bitmap cap
// A run can be assembled from repeated 2560-makeups; cap the accumulated
// length (and thus the loop) at the columns cap plus one extra code.
#define CCITT_MAX_RUN (CCITT_MAX_COLUMNS + 2560)

// --- Canonical T.4 code tables (white/black terminating + makeup) ---

typedef struct {
    uint16_t run;   // run length this code stands for
    uint8_t len;    // code length in bits
    uint16_t code;  // the code, MSB-justified within `len` bits
} CcittCode;

static const CcittCode ccitt_white[] = {
    // Terminating codes 0..63.
    {0, 8, 0x35},   {1, 6, 0x07},   {2, 4, 0x07},   {3, 4, 0x08},
    {4, 4, 0x0B},   {5, 4, 0x0C},   {6, 4, 0x0E},   {7, 4, 0x0F},
    {8, 5, 0x13},   {9, 5, 0x14},   {10, 5, 0x07},  {11, 5, 0x08},
    {12, 6, 0x08},  {13, 6, 0x03},  {14, 6, 0x34},  {15, 6, 0x35},
    {16, 6, 0x2A},  {17, 6, 0x2B},  {18, 7, 0x27},  {19, 7, 0x0C},
    {20, 7, 0x08},  {21, 7, 0x17},  {22, 7, 0x03},  {23, 7, 0x04},
    {24, 7, 0x28},  {25, 7, 0x2B},  {26, 7, 0x13},  {27, 7, 0x24},
    {28, 7, 0x18},  {29, 8, 0x02},  {30, 8, 0x03},  {31, 8, 0x1A},
    {32, 8, 0x1B},  {33, 8, 0x12},  {34, 8, 0x13},  {35, 8, 0x14},
    {36, 8, 0x15},  {37, 8, 0x16},  {38, 8, 0x17},  {39, 8, 0x28},
    {40, 8, 0x29},  {41, 8, 0x2A},  {42, 8, 0x2B},  {43, 8, 0x2C},
    {44, 8, 0x2D},  {45, 8, 0x04},  {46, 8, 0x05},  {47, 8, 0x0A},
    {48, 8, 0x0B},  {49, 8, 0x52},  {50, 8, 0x53},  {51, 8, 0x54},
    {52, 8, 0x55},  {53, 8, 0x24},  {54, 8, 0x25},  {55, 8, 0x58},
    {56, 8, 0x59},  {57, 8, 0x5A},  {58, 8, 0x5B},  {59, 8, 0x4A},
    {60, 8, 0x4B},  {61, 8, 0x32},  {62, 8, 0x33},  {63, 8, 0x34},
    // Makeup codes 64..1728.
    {64, 5, 0x1B},   {128, 5, 0x12},  {192, 6, 0x17},  {256, 7, 0x37},
    {320, 8, 0x36},  {384, 8, 0x37},  {448, 8, 0x64},  {512, 8, 0x65},
    {576, 8, 0x68},  {640, 8, 0x67},  {704, 9, 0xCC},  {768, 9, 0xCD},
    {832, 9, 0xD2},  {896, 9, 0xD3},  {960, 9, 0xD4},  {1024, 9, 0xD5},
    {1088, 9, 0xD6}, {1152, 9, 0xD7}, {1216, 9, 0xD8}, {1280, 9, 0xD9},
    {1344, 9, 0xDA}, {1408, 9, 0xDB}, {1472, 9, 0x98}, {1536, 9, 0x99},
    {1600, 9, 0x9A}, {1664, 6, 0x18}, {1728, 9, 0x9B},
};

static const CcittCode ccitt_black[] = {
    // Terminating codes 0..63.
    {0, 10, 0x37},   {1, 3, 0x02},    {2, 2, 0x03},    {3, 2, 0x02},
    {4, 3, 0x03},    {5, 4, 0x03},    {6, 4, 0x02},    {7, 5, 0x03},
    {8, 6, 0x05},    {9, 6, 0x04},    {10, 7, 0x04},   {11, 7, 0x05},
    {12, 7, 0x07},   {13, 8, 0x04},   {14, 8, 0x07},   {15, 9, 0x18},
    {16, 10, 0x17},  {17, 10, 0x18},  {18, 10, 0x08},  {19, 11, 0x67},
    {20, 11, 0x68},  {21, 11, 0x6C},  {22, 11, 0x37},  {23, 11, 0x28},
    {24, 11, 0x17},  {25, 11, 0x18},  {26, 12, 0xCA},  {27, 12, 0xCB},
    {28, 12, 0xCC},  {29, 12, 0xCD},  {30, 12, 0x68},  {31, 12, 0x69},
    {32, 12, 0x6A},  {33, 12, 0x6B},  {34, 12, 0xD2},  {35, 12, 0xD3},
    {36, 12, 0xD4},  {37, 12, 0xD5},  {38, 12, 0xD6},  {39, 12, 0xD7},
    {40, 12, 0x6C},  {41, 12, 0x6D},  {42, 12, 0xDA},  {43, 12, 0xDB},
    {44, 12, 0x54},  {45, 12, 0x55},  {46, 12, 0x56},  {47, 12, 0x57},
    {48, 12, 0x64},  {49, 12, 0x65},  {50, 12, 0x52},  {51, 12, 0x53},
    {52, 12, 0x24},  {53, 12, 0x37},  {54, 12, 0x38},  {55, 12, 0x27},
    {56, 12, 0x28},  {57, 12, 0x58},  {58, 12, 0x59},  {59, 12, 0x2B},
    {60, 12, 0x2C},  {61, 12, 0x5A},  {62, 12, 0x66},  {63, 12, 0x67},
    // Makeup codes 64..1728.
    {64, 10, 0x0F},   {128, 12, 0xC8},  {192, 12, 0xC9},  {256, 12, 0x5B},
    {320, 12, 0x33},  {384, 12, 0x34},  {448, 12, 0x35},  {512, 13, 0x6C},
    {576, 13, 0x6D},  {640, 13, 0x4A},  {704, 13, 0x4B},  {768, 13, 0x4C},
    {832, 13, 0x4D},  {896, 13, 0x72},  {960, 13, 0x73},  {1024, 13, 0x74},
    {1088, 13, 0x75}, {1152, 13, 0x76}, {1216, 13, 0x77}, {1280, 13, 0x52},
    {1344, 13, 0x53}, {1408, 13, 0x54}, {1472, 13, 0x55}, {1536, 13, 0x5A},
    {1600, 13, 0x5B}, {1664, 13, 0x64}, {1728, 13, 0x65},
};

// Extended makeup codes 1792..2560, shared by both colors.
static const CcittCode ccitt_ext[] = {
    {1792, 11, 0x08}, {1856, 11, 0x0C}, {1920, 11, 0x0D}, {1984, 12, 0x12},
    {2048, 12, 0x13}, {2112, 12, 0x14}, {2176, 12, 0x15}, {2240, 12, 0x16},
    {2304, 12, 0x17}, {2368, 12, 0x1C}, {2432, 12, 0x1D}, {2496, 12, 0x1E},
    {2560, 12, 0x1F},
};

#define CCITT_NWHITE (sizeof(ccitt_white) / sizeof(ccitt_white[0]))
#define CCITT_NBLACK (sizeof(ccitt_black) / sizeof(ccitt_black[0]))
#define CCITT_NEXT (sizeof(ccitt_ext) / sizeof(ccitt_ext[0]))

// --- Decode lookup tables (peek N bits -> run + code length) ---

#define CCITT_WHITE_BITS 12  // longest white code incl. extended makeup
#define CCITT_BLACK_BITS 13  // longest black code

typedef struct {
    int16_t run;  // -1 = invalid
    uint8_t len;
} CcittLutEntry;

static CcittLutEntry ccitt_white_lut[1 << CCITT_WHITE_BITS];
static CcittLutEntry ccitt_black_lut[1 << CCITT_BLACK_BITS];
static bool ccitt_lut_ready = false;

static void ccitt_lut_fill(CcittLutEntry *lut, int lut_bits,
                           const CcittCode *codes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const CcittCode *c = &codes[i];
        int shift = lut_bits - c->len;
        uint32_t base = (uint32_t)c->code << shift;
        for (uint32_t f = 0; f < (1u << shift); f++) {
            lut[base + f].run = (int16_t)c->run;
            lut[base + f].len = c->len;
        }
    }
}

static void ccitt_lut_init(void) {
    // Idempotent; a benign race writes identical values.
    if (ccitt_lut_ready) return;
    for (size_t i = 0; i < (1u << CCITT_WHITE_BITS); i++)
        ccitt_white_lut[i].run = -1;
    for (size_t i = 0; i < (1u << CCITT_BLACK_BITS); i++)
        ccitt_black_lut[i].run = -1;
    ccitt_lut_fill(ccitt_white_lut, CCITT_WHITE_BITS, ccitt_white, CCITT_NWHITE);
    ccitt_lut_fill(ccitt_white_lut, CCITT_WHITE_BITS, ccitt_ext, CCITT_NEXT);
    ccitt_lut_fill(ccitt_black_lut, CCITT_BLACK_BITS, ccitt_black, CCITT_NBLACK);
    ccitt_lut_fill(ccitt_black_lut, CCITT_BLACK_BITS, ccitt_ext, CCITT_NEXT);
    ccitt_lut_ready = true;
}

// --- Bit reader (MSB-first, zero-fill past end, exhaustion tracked) ---

typedef struct {
    const uint8_t *data;
    size_t nbits;  // total bits available
    size_t pos;    // current bit position
} CcittBits;

// Peek up to 24 bits (callers use at most 13). Loads a 32-bit window and
// zero-fills past the end, so it is branch-light on the hot path.
static uint32_t ccitt_peek(const CcittBits *b, int n) {
    size_t byte = b->pos >> 3;
    size_t nbytes = (b->nbits + 7) >> 3;
    uint32_t v;
    if (byte + 4 <= nbytes) {
        v = ((uint32_t)b->data[byte] << 24) | ((uint32_t)b->data[byte + 1] << 16) |
            ((uint32_t)b->data[byte + 2] << 8) | (uint32_t)b->data[byte + 3];
    } else {
        v = 0;
        for (size_t i = 0; i < 4; i++)
            if (byte + i < nbytes)
                v |= (uint32_t)b->data[byte + i] << (24 - 8 * i);
    }
    v <<= b->pos & 7;  // n + intra-byte shift <= 31, so no bits are lost
    return v >> (32 - n);
}

static void ccitt_align(CcittBits *b) {
    b->pos = (b->pos + 7) & ~(size_t)7;
}

// Bits left, clamped: enough to compare against small thresholds.
static size_t ccitt_left(const CcittBits *b) {
    return b->pos < b->nbits ? b->nbits - b->pos : 0;
}

// --- Run decoding ---

enum {
    CCITT_RUN_ERR = -1,
    CCITT_RUN_EOL = -2,  // eleven-plus zeros ahead: fill/EOL territory
};

// Decode one complete run (makeup chain + terminating code) of `color`
// (0 = white, 1 = black). Returns the run length, CCITT_RUN_EOL when the
// next bits are EOL/fill (nothing consumed), or CCITT_RUN_ERR.
static long ccitt_run(CcittBits *b, int color) {
    long total = 0;
    for (;;) {
        if (ccitt_left(b) == 0) return CCITT_RUN_ERR;
        if (ccitt_peek(b, 11) == 0) return total == 0 ? CCITT_RUN_EOL : CCITT_RUN_ERR;
        CcittLutEntry e;
        if (color) e = ccitt_black_lut[ccitt_peek(b, CCITT_BLACK_BITS)];
        else e = ccitt_white_lut[ccitt_peek(b, CCITT_WHITE_BITS)];
        if (e.run < 0 || (size_t)e.len > ccitt_left(b)) return CCITT_RUN_ERR;
        b->pos += e.len;
        total += e.run;
        if (total > CCITT_MAX_RUN) return CCITT_RUN_ERR;
        if (e.run < 64) return total;  // terminating code ends the run
    }
}

// Consume optional zero fill followed by an EOL (000000000001). Returns
// false (position unchanged) when no EOL is found within the fill budget.
static bool ccitt_eol(CcittBits *b) {
    size_t save = b->pos;
    size_t zeros = 0;
    while (ccitt_left(b) > 0 && ccitt_peek(b, 1) == 0) {
        b->pos++;
        if (++zeros > 4096) {  // fill budget: nothing sane pads this much
            b->pos = save;
            return false;
        }
    }
    if (zeros >= 11 && ccitt_left(b) > 0) {
        b->pos++;  // the terminating 1 bit
        return true;
    }
    b->pos = save;
    return false;
}

// --- Line decoding into changing-element arrays ---
//
// chg[] holds the positions where the pixel color flips, strictly
// increasing, 0..columns; a line starts white. chg[2i] is a white->black
// transition, chg[2i+1] black->white. `n` entries are valid.

typedef struct {
    int *chg;
    int n;
} CcittLine;

// b1 = first changing element on the reference line to the right of a0 with
// the same color-transition sense as the current color; b2 = the next one.
// *ri is a monotonically advancing index hint.
static void ccitt_ref_pair(const CcittLine *ref, int a0, int color, int columns,
                           int *ri, int *b1, int *b2) {
    int i = *ri;
    if (i > ref->n) i = ref->n;
    // Back up if the hint overshot (pass mode moves a0 without consuming
    // current-line changes, so the hint only ever lags; this is defensive).
    while (i > 0 && ref->chg[i - 1] > a0) i--;
    while (i < ref->n && (ref->chg[i] <= a0 || ((i & 1) != color))) i++;
    *ri = i;
    *b1 = i < ref->n ? ref->chg[i] : columns;
    *b2 = i + 1 < ref->n ? ref->chg[i + 1] : columns;
}

// Decode one 2-D (T.6-style) coded line against `ref` into `cur`.
// Returns false on malformed data.
static bool ccitt_decode_line_2d(CcittBits *b, const CcittLine *ref,
                                 CcittLine *cur, int columns) {
    int a0 = -1;
    int color = 0;
    int ri = 0;
    cur->n = 0;

    while (a0 < columns) {
        if (ccitt_left(b) == 0) return false;
        int b1, b2;
        ccitt_ref_pair(ref, a0, color, columns, &ri, &b1, &b2);

        uint32_t v7 = ccitt_peek(b, 7);
        if (v7 & 0x40) {  // 1xxxxxx: V0
            b->pos += 1;
            int a1 = b1;
            if (a1 < 0 || a1 > columns || (a0 >= 0 && a1 <= a0)) return false;
            if (cur->n >= columns + 2) return false;
            cur->chg[cur->n++] = a1;
            a0 = a1;
            color ^= 1;
        } else if ((v7 >> 4) == 0x3 || (v7 >> 4) == 0x2) {  // 011 VR1 / 010 VL1
            b->pos += 3;
            int a1 = (v7 >> 4) == 0x3 ? b1 + 1 : b1 - 1;
            if (a1 < 0 || a1 > columns || (a0 >= 0 && a1 <= a0)) return false;
            if (cur->n >= columns + 2) return false;
            cur->chg[cur->n++] = a1;
            a0 = a1;
            color ^= 1;
        } else if ((v7 >> 4) == 0x1) {  // 001: horizontal
            b->pos += 3;
            long r1 = ccitt_run(b, color);
            if (r1 < 0) return false;
            long r2 = ccitt_run(b, color ^ 1);
            if (r2 < 0) return false;
            long start = a0 < 0 ? 0 : a0;
            long a1 = start + r1;
            long a2 = a1 + r2;
            if (a2 > columns) return false;
            if (a0 >= 0 && a2 <= a0) return false;  // no forward progress
            if (a0 < 0 && a2 == 0) return false;
            if (cur->n + 2 > columns + 2) return false;
            cur->chg[cur->n++] = (int)a1;
            cur->chg[cur->n++] = (int)a2;
            a0 = (int)a2;
            // color unchanged: two transitions
        } else if ((v7 >> 3) == 0x1) {  // 0001: pass
            b->pos += 4;
            if (b2 <= a0 && a0 >= 0) return false;
            a0 = b2;
        } else if ((v7 >> 1) == 0x3) {  // 000011: VR2
            b->pos += 6;
            int a1 = b1 + 2;
            if (a1 < 0 || a1 > columns || (a0 >= 0 && a1 <= a0)) return false;
            if (cur->n >= columns + 2) return false;
            cur->chg[cur->n++] = a1;
            a0 = a1;
            color ^= 1;
        } else if ((v7 >> 1) == 0x2) {  // 000010: VL2
            b->pos += 6;
            int a1 = b1 - 2;
            if (a1 < 0 || a1 > columns || (a0 >= 0 && a1 <= a0)) return false;
            if (cur->n >= columns + 2) return false;
            cur->chg[cur->n++] = a1;
            a0 = a1;
            color ^= 1;
        } else if (v7 == 0x3 || v7 == 0x2) {  // 0000011 VR3 / 0000010 VL3
            b->pos += 7;
            int a1 = v7 == 0x3 ? b1 + 3 : b1 - 3;
            if (a1 < 0 || a1 > columns || (a0 >= 0 && a1 <= a0)) return false;
            if (cur->n >= columns + 2) return false;
            cur->chg[cur->n++] = a1;
            a0 = a1;
            color ^= 1;
        } else {
            // 0000000 or 0000001: EOL / extension (uncompressed mode) /
            // garbage — all end this line's decode as an error; the row
            // loop handles legal EOL/EOFB before calling us.
            return false;
        }
    }
    return a0 == columns;
}

// Decode one 1-D (MH) coded line into `cur`.
static bool ccitt_decode_line_1d(CcittBits *b, CcittLine *cur, int columns) {
    int pos = 0;
    int color = 0;
    cur->n = 0;
    // Each iteration consumes at least one code (>= 2 bits), and `pos` can
    // stall only on zero-length runs, so cap the iterations.
    for (int iter = 0; pos < columns; iter++) {
        if (iter > columns + 64) return false;
        long r = ccitt_run(b, color);
        if (r == CCITT_RUN_EOL) return false;  // premature EOL mid-line
        if (r < 0) return false;
        pos += (int)r;
        if (pos > columns) return false;
        // Record the transition after every run (including one landing
        // exactly on `columns`): entries then alternate white->black at
        // even indices, black->white at odd — the parity the 2-D reference
        // lookup depends on.
        if (cur->n >= columns + 2) return false;
        cur->chg[cur->n++] = pos;
        color ^= 1;
    }
    return true;
}

// Render a changing-element line to one output row of 0/255 bytes.
static void ccitt_render_line(const CcittLine *ln, int columns, bool black_is_1,
                              uint8_t *row) {
    uint8_t white = black_is_1 ? 0 : 255;
    uint8_t black = black_is_1 ? 255 : 0;
    memset(row, white, (size_t)columns);
    for (int i = 0; i + 1 < ln->n; i += 2) {
        int s = ln->chg[i];
        int e = ln->chg[i + 1];
        if (s < 0) s = 0;
        if (e > columns) e = columns;
        if (e > s) memset(row + s, black, (size_t)(e - s));
    }
    if (ln->n & 1) {  // trailing black span to the right edge
        int s = ln->chg[ln->n - 1];
        if (s < 0) s = 0;
        if (s < columns) memset(row + s, black, (size_t)(columns - s));
    }
}

void tspdf_ccitt_params_default(TspdfCcittParams *p) {
    if (!p) return;
    p->k = 0;
    p->columns = 1728;
    p->rows = 0;
    p->black_is_1 = false;
    p->encoded_byte_align = false;
    p->end_of_line = false;
    p->end_of_block = true;
}

bool tspdf_ccitt_decode(const uint8_t *data, size_t len,
                        const TspdfCcittParams *params, TspdfArena *arena,
                        TspdfCcittBitmap *out) {
    if (!data || !params || !arena || !out) return false;
    int columns = params->columns;
    int rows = params->rows;
    if (columns < 1 || columns > CCITT_MAX_COLUMNS) return false;
    if (rows < 0 || rows > CCITT_MAX_ROWS) return false;
    if (rows > 0 && (size_t)columns * (size_t)rows > CCITT_MAX_PIXELS)
        return false;
    if (len > SIZE_MAX / 8) return false;

    ccitt_lut_init();

    CcittBits bits = {data, len * 8, 0};

    // Two changing-element lines (reference + current), swapped per row.
    int *chg_a = (int *)malloc(((size_t)columns + 4) * sizeof(int));
    int *chg_b = (int *)malloc(((size_t)columns + 4) * sizeof(int));
    // Row store: exact when rows is known, grown otherwise.
    size_t row_cap = rows > 0 ? (size_t)rows : 64;
    uint8_t *store = (uint8_t *)malloc(row_cap * (size_t)columns);
    if (!chg_a || !chg_b || !store) {
        free(chg_a);
        free(chg_b);
        free(store);
        return false;
    }

    CcittLine ref = {chg_a, 0};  // imaginary all-white line above row 0
    CcittLine cur = {chg_b, 0};
    int row = 0;
    bool ok = true;
    bool done = false;
    // K > 0: 1-D/2-D tag bit follows each EOL (and, EOL-less, prefixes each
    // line). The first line of a K > 0 stream is 1-D by construction.
    while (!done && (rows == 0 || row < rows)) {
        if (params->encoded_byte_align) ccitt_align(&bits);

        if (ccitt_left(&bits) == 0) break;  // clean end of data

        bool line_is_1d;
        if (params->k < 0) {
            // G4: EOL here can only start EOFB.
            if (ccitt_peek(&bits, 12) == 1) {
                done = true;
                break;
            }
            line_is_1d = false;
        } else {
            // G3: optional fill + EOL before each line. A second EOL right
            // after the first is RTC (end of data).
            bool saw_eol = ccitt_eol(&bits);
            if (saw_eol && (ccitt_left(&bits) == 0 || ccitt_peek(&bits, 12) == 1)) {
                done = true;
                break;
            }
            if (params->k > 0) {
                if (ccitt_left(&bits) == 0) break;
                if (saw_eol) {
                    // The 1-D/2-D tag bit follows each EOL (T.4 4.2.1.3.4).
                    line_is_1d = ccitt_peek(&bits, 1) == 1;
                    bits.pos += 1;
                } else {
                    // No EOL, no tag: encoders that omit EOLs (for example
                    // Ghostscript's CCITTFaxEncode with EndOfLine false)
                    // code exactly one 1-D line every K lines, so the
                    // cadence is the only — and sufficient — signal.
                    line_is_1d = (row % params->k) == 0;
                }
            } else {
                line_is_1d = true;
            }
            if (ccitt_left(&bits) == 0) break;
        }

        if (line_is_1d) ok = ccitt_decode_line_1d(&bits, &cur, columns);
        else ok = ccitt_decode_line_2d(&bits, &ref, &cur, columns);
        if (!ok) {
            // When the row count is unknown, trailing garbage after at least
            // one good row ends the image instead of failing it (mirrors
            // how viewers tolerate sloppy real-world streams). With /Rows
            // given, a short decode is a hard error.
            if (rows == 0 && row > 0) {
                ok = true;
                done = true;
                break;
            }
            break;
        }

        if ((size_t)row >= row_cap) {
            size_t max_rows = CCITT_MAX_PIXELS / (size_t)columns;
            if (max_rows > CCITT_MAX_ROWS) max_rows = CCITT_MAX_ROWS;
            size_t ncap = row_cap * 2;
            if (ncap > max_rows) ncap = max_rows;
            if (ncap <= row_cap) {
                ok = false;
                break;
            }
            uint8_t *ns = (uint8_t *)realloc(store, ncap * (size_t)columns);
            if (!ns) {
                ok = false;
                break;
            }
            store = ns;
            row_cap = ncap;
        }
        if ((size_t)(row + 1) * (size_t)columns > CCITT_MAX_PIXELS) {
            ok = false;
            break;
        }
        ccitt_render_line(&cur, columns, params->black_is_1,
                          store + (size_t)row * (size_t)columns);
        row++;
        if (row > CCITT_MAX_ROWS) {
            ok = false;
            break;
        }

        // Current line becomes the reference for the next.
        CcittLine tmp = ref;
        ref = cur;
        cur = tmp;
    }

    if (ok && rows > 0 && row < rows) ok = false;  // short image
    if (ok && row == 0) ok = false;                // nothing decoded

    uint8_t *pixels = NULL;
    if (ok) {
        pixels = (uint8_t *)tspdf_arena_alloc(arena, (size_t)row * (size_t)columns);
        if (!pixels) ok = false;
        else memcpy(pixels, store, (size_t)row * (size_t)columns);
    }

    free(chg_a);
    free(chg_b);
    free(store);
    if (!ok) return false;

    out->width = columns;
    out->height = row;
    out->pixels = pixels;
    return true;
}

// --- G4 encoder ---

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t bitpos;
    bool oom;
} CcittWriter;

static void ccitt_put(CcittWriter *w, uint32_t code, int len) {
    if (w->oom) return;
    size_t need = (w->bitpos + (size_t)len + 7) / 8;
    if (need > w->cap) {
        size_t ncap = w->cap ? w->cap * 2 : 256;
        while (ncap < need) ncap *= 2;
        uint8_t *nb = (uint8_t *)realloc(w->buf, ncap);
        if (!nb) {
            w->oom = true;
            return;
        }
        memset(nb + w->cap, 0, ncap - w->cap);
        w->buf = nb;
        w->cap = ncap;
    }
    for (int i = len - 1; i >= 0; i--) {
        if ((code >> i) & 1u)
            w->buf[w->bitpos >> 3] |= (uint8_t)(0x80u >> (w->bitpos & 7));
        w->bitpos++;
    }
}

// Emit the MH code(s) for a run of `color` (0 = white): repeated 2560
// makeups for very long runs, one makeup for 64..2560, then a terminator.
static void ccitt_put_run(CcittWriter *w, long run, int color) {
    const CcittCode *codes = color ? ccitt_black : ccitt_white;
    while (run > 2560 + 63) {
        ccitt_put(w, ccitt_ext[CCITT_NEXT - 1].code, ccitt_ext[CCITT_NEXT - 1].len);
        run -= 2560;
    }
    if (run >= 64) {
        long makeup = run & ~63L;
        const CcittCode *m;
        if (makeup >= 1792) m = &ccitt_ext[(makeup - 1792) / 64];
        else m = &codes[64 + (makeup / 64 - 1)];
        ccitt_put(w, m->code, m->len);
        run -= makeup;
    }
    ccitt_put(w, codes[run].code, codes[run].len);
}

// Fill chg[] with the changing-element positions of a byte row (< 128 =
// black). Returns the count.
static int ccitt_find_changes(const uint8_t *row, int columns, int *chg) {
    int n = 0;
    int color = 0;  // white
    for (int x = 0; x < columns; x++) {
        int px = row[x] < 128 ? 1 : 0;
        if (px != color) {
            chg[n++] = x;
            color = px;
        }
    }
    return n;
}

bool tspdf_ccitt_encode_g4(const uint8_t *pixels, int width, int height,
                           TspdfArena *arena, uint8_t **out, size_t *out_len) {
    if (!pixels || !arena || !out || !out_len) return false;
    if (width < 1 || width > CCITT_MAX_COLUMNS || height < 1 ||
        height > CCITT_MAX_ROWS)
        return false;
    if ((size_t)width * (size_t)height > CCITT_MAX_PIXELS) return false;

    int *chg_ref = (int *)malloc(((size_t)width + 4) * sizeof(int));
    int *chg_cur = (int *)malloc(((size_t)width + 4) * sizeof(int));
    if (!chg_ref || !chg_cur) {
        free(chg_ref);
        free(chg_cur);
        return false;
    }

    CcittWriter w = {NULL, 0, 0, false};
    CcittLine ref = {chg_ref, 0};  // imaginary all-white reference line
    CcittLine cur = {chg_cur, 0};

    for (int y = 0; y < height; y++) {
        cur.n = ccitt_find_changes(pixels + (size_t)y * (size_t)width, width,
                                   cur.chg);
        int a0 = -1;
        int color = 0;
        int ci = 0;  // index into cur.chg of the next change with parity==color
        int ri = 0;

        while (a0 < width) {
            // a1: next change on this line after a0 with the right parity.
            while (ci < cur.n && (cur.chg[ci] <= a0 || ((ci & 1) != color))) ci++;
            int a1 = ci < cur.n ? cur.chg[ci] : width;
            int b1, b2;
            ccitt_ref_pair(&ref, a0, color, width, &ri, &b1, &b2);

            if (b2 < a1) {
                ccitt_put(&w, 0x1, 4);  // pass: 0001
                a0 = b2;
            } else if (a1 - b1 >= -3 && a1 - b1 <= 3) {
                static const struct { uint16_t code; uint8_t len; } vcode[7] = {
                    {0x02, 7},  // VL3 0000010
                    {0x02, 6},  // VL2 000010
                    {0x02, 3},  // VL1 010
                    {0x01, 1},  // V0  1
                    {0x03, 3},  // VR1 011
                    {0x03, 6},  // VR2 000011
                    {0x03, 7},  // VR3 0000011
                };
                int d = a1 - b1 + 3;
                ccitt_put(&w, vcode[d].code, vcode[d].len);
                a0 = a1;
                color ^= 1;
                ci++;
            } else {
                // Horizontal: two runs from max(a0,0).
                int a2 = ci + 1 < cur.n ? cur.chg[ci + 1] : width;
                if (a1 >= width) a2 = width;  // no second change: run to edge
                int start = a0 < 0 ? 0 : a0;
                ccitt_put(&w, 0x1, 3);  // 001
                ccitt_put_run(&w, a1 - start, color);
                ccitt_put_run(&w, a2 - a1, color ^ 1);
                a0 = a2;
                ci += 2;
            }
        }

        // Swap: current becomes reference.
        CcittLine tmp = ref;
        ref = cur;
        cur = tmp;
    }

    // EOFB: two EOLs, then pad to a byte with zeros (writer buffer is
    // zero-filled, so padding is just rounding the length up).
    ccitt_put(&w, 0x001, 12);
    ccitt_put(&w, 0x001, 12);

    free(chg_ref);
    free(chg_cur);
    if (w.oom) {
        free(w.buf);
        return false;
    }

    size_t nbytes = (w.bitpos + 7) / 8;
    uint8_t *res = (uint8_t *)tspdf_arena_alloc(arena, nbytes ? nbytes : 1);
    if (!res) {
        free(w.buf);
        return false;
    }
    memcpy(res, w.buf, nbytes);
    free(w.buf);
    *out = res;
    *out_len = nbytes;
    return true;
}
