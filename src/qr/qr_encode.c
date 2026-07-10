/*
 * qr_encode.c — Minimal QR code encoder.
 * Written from scratch for the tspdf project.
 * Implements QR Code Model 2, byte mode, ECC levels L/M/Q/H, versions 1-11.
 * No external dependencies.
 *
 * Implements the core QR code standard (ISO/IEC 18004) steps:
 *   1. Version selection
 *   2. Data encoding (byte mode)
 *   3. Reed-Solomon error correction
 *   4. Matrix construction (finder, timing, alignment, format info,
 *      version info for versions >= 7)
 *   5. Data placement (zigzag)
 *   6. Mask pattern selection (all 8, lowest penalty)
 */

#include "qr_encode.h"
#include <stdlib.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define MAX_VERSION 11
#define MAX_MODULES 61  /* version 11: 17 + 4*11 = 61 */

/*
 * Total codewords and ECC block structure per version and level.
 * Fields: total_cw, ecc_per_block, blocks_g1, data_per_block_g1,
 *         blocks_g2, data_per_block_g2
 *
 * Source: ISO/IEC 18004:2015, Table 9; every row cross-checked against
 * segno's tables at dev time (blocks x (data + ec) == total codewords,
 * and the derived byte-mode character capacity matches Table 7).
 */
typedef struct {
    int total_cw;
    int ecc_per_block;
    int blocks_g1;
    int data_g1;
    int blocks_g2;
    int data_g2;
} EccInfo;

static const EccInfo ECC_INFO[4][MAX_VERSION + 1] = {
    [QR_EC_L] = {
        {  0,  0, 0,   0, 0,  0}, /* v0 placeholder */
        { 26,  7, 1,  19, 0,  0}, /* v1 */
        { 44, 10, 1,  34, 0,  0}, /* v2 */
        { 70, 15, 1,  55, 0,  0}, /* v3 */
        {100, 20, 1,  80, 0,  0}, /* v4 */
        {134, 26, 1, 108, 0,  0}, /* v5 */
        {172, 18, 2,  68, 0,  0}, /* v6 */
        {196, 20, 2,  78, 0,  0}, /* v7 */
        {242, 24, 2,  97, 0,  0}, /* v8 */
        {292, 30, 2, 116, 0,  0}, /* v9 */
        {346, 18, 2,  68, 2, 69}, /* v10 */
        {404, 20, 4,  81, 0,  0}, /* v11 */
    },
    [QR_EC_M] = {
        {  0,  0, 0,  0, 0,  0}, /* v0 placeholder */
        { 26, 10, 1, 16, 0,  0}, /* v1 */
        { 44, 16, 1, 28, 0,  0}, /* v2 */
        { 70, 26, 1, 44, 0,  0}, /* v3 */
        {100, 18, 2, 32, 0,  0}, /* v4 */
        {134, 24, 2, 43, 0,  0}, /* v5 */
        {172, 16, 4, 27, 0,  0}, /* v6 */
        {196, 18, 4, 31, 0,  0}, /* v7 */
        {242, 22, 2, 38, 2, 39}, /* v8 */
        {292, 22, 3, 36, 2, 37}, /* v9 */
        {346, 26, 4, 43, 1, 44}, /* v10 */
        {404, 30, 1, 50, 4, 51}, /* v11 */
    },
    [QR_EC_Q] = {
        {  0,  0, 0,  0, 0,  0}, /* v0 placeholder */
        { 26, 13, 1, 13, 0,  0}, /* v1 */
        { 44, 22, 1, 22, 0,  0}, /* v2 */
        { 70, 18, 2, 17, 0,  0}, /* v3 */
        {100, 26, 2, 24, 0,  0}, /* v4 */
        {134, 18, 2, 15, 2, 16}, /* v5 */
        {172, 24, 4, 19, 0,  0}, /* v6 */
        {196, 18, 2, 14, 4, 15}, /* v7 */
        {242, 22, 4, 18, 2, 19}, /* v8 */
        {292, 20, 4, 16, 4, 17}, /* v9 */
        {346, 24, 6, 19, 2, 20}, /* v10 */
        {404, 28, 4, 22, 4, 23}, /* v11 */
    },
    [QR_EC_H] = {
        {  0,  0, 0,  0, 0,  0}, /* v0 placeholder */
        { 26, 17, 1,  9, 0,  0}, /* v1 */
        { 44, 28, 1, 16, 0,  0}, /* v2 */
        { 70, 22, 2, 13, 0,  0}, /* v3 */
        {100, 16, 4,  9, 0,  0}, /* v4 */
        {134, 22, 2, 11, 2, 12}, /* v5 */
        {172, 28, 4, 15, 0,  0}, /* v6 */
        {196, 26, 4, 13, 1, 14}, /* v7 */
        {242, 26, 4, 14, 2, 15}, /* v8 */
        {292, 24, 4, 12, 4, 13}, /* v9 */
        {346, 28, 6, 15, 2, 16}, /* v10 */
        {404, 24, 3, 12, 8, 13}, /* v11 */
    },
};

/*
 * Byte-mode character capacity, derived from the block structure: data
 * bits minus the 4-bit mode indicator and the character count field.
 * Matches ISO/IEC 18004:2015 Table 7 for every version/level (the
 * terminator may be truncated when the data fills the symbol, so it does
 * not reserve space). Returns 0 for out-of-range arguments.
 */
static int data_capacity(int version, QrEcLevel level) {
    if (version < 1 || version > MAX_VERSION ||
        (int)level < 0 || (int)level > 3) return 0;
    const EccInfo *ei = &ECC_INFO[level][version];
    int data_cw = ei->blocks_g1 * ei->data_g1 + ei->blocks_g2 * ei->data_g2;
    int cc_bits = (version <= 9) ? 8 : 16;
    return (data_cw * 8 - 4 - cc_bits) / 8;
}

/* Alignment pattern centers for versions 1-11 (version 1 has none) */
/* Each row: list of centers terminated by -1 */
static const int ALIGN_CENTERS[MAX_VERSION + 1][7] = {
    {-1},              /* v0 */
    {-1},              /* v1: no alignment patterns */
    {6, 18, -1},       /* v2 */
    {6, 22, -1},       /* v3 */
    {6, 26, -1},       /* v4 */
    {6, 30, -1},       /* v5 */
    {6, 34, -1},       /* v6 */
    {6, 22, 38, -1},   /* v7 */
    {6, 24, 42, -1},   /* v8 */
    {6, 26, 46, -1},   /* v9 */
    {6, 28, 50, -1},   /* v10 */
    {6, 30, 54, -1},   /* v11 */
};

/* ── GF(256) Arithmetic ─────────────────────────────────────────────────── */

static uint8_t gf_exp[256];
static uint8_t gf_log[256];
static int     gf_initialized = 0;

static void gf_init(void)
{
    if (gf_initialized) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    gf_exp[255] = gf_exp[0]; /* wrap for convenience */
    gf_initialized = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return gf_exp[(gf_log[a] + gf_log[b]) % 255];
}

/* ── Reed-Solomon ECC ───────────────────────────────────────────────────── */

/*
 * Generate `num_ecc` error correction codewords for `data` of length `data_len`.
 * Uses polynomial long division in GF(256).
 */
static void rs_generate(const uint8_t *data, int data_len,
                         uint8_t *ecc, int num_ecc)
{
    /* Build generator polynomial g(x) = prod_{i=0}^{num_ecc-1} (x - alpha^i) */
    uint8_t gen[64];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (int i = 0; i < num_ecc; i++) {
        /* Multiply gen by (x - alpha^i), i.e. (x XOR alpha^i) */
        for (int j = i; j >= 0; j--) {
            gen[j + 1] ^= gf_mul(gen[j], gf_exp[i]);
        }
    }
    /* gen[0..num_ecc] now holds coefficients (leading term at gen[0]=1 implied) */

    /* Polynomial division: remainder after dividing data poly by gen */
    uint8_t rem[64];
    memset(rem, 0, sizeof(rem));
    for (int i = 0; i < data_len; i++) {
        uint8_t coef = data[i] ^ rem[0];
        /* Shift remainder left */
        memmove(rem, rem + 1, (size_t)(num_ecc - 1));
        rem[num_ecc - 1] = 0;
        if (coef != 0) {
            for (int j = 0; j < num_ecc; j++) {
                rem[j] ^= gf_mul(gen[j + 1], coef);
            }
        }
    }
    memcpy(ecc, rem, (size_t)num_ecc);
}

/* ── Data Encoding ──────────────────────────────────────────────────────── */

/* Bit buffer for encoding */
typedef struct {
    uint8_t data[4096];
    int     bit_len;
} BitBuf;

static void bb_append(BitBuf *bb, unsigned int value, int bits)
{
    for (int i = bits - 1; i >= 0; i--) {
        int byte_idx = bb->bit_len / 8;
        int bit_idx  = 7 - (bb->bit_len % 8);
        if ((value >> i) & 1)
            bb->data[byte_idx] |= (uint8_t)(1 << bit_idx);
        bb->bit_len++;
    }
}

/*
 * Encode `text` into a codeword sequence for the given version/block
 * structure using byte mode. Returns number of data codewords written,
 * or -1 on error.
 */
static int encode_data(const char *text, int text_len, int version,
                        const EccInfo *ei, uint8_t *out, int out_size)
{
    BitBuf bb;
    memset(&bb, 0, sizeof(bb));

    /* Mode indicator: 0100 = byte mode */
    bb_append(&bb, 0x4, 4);

    /* Character count indicator: 8 bits for versions 1-9, 16 for 10+ */
    int cc_bits = (version <= 9) ? 8 : 16;
    bb_append(&bb, (unsigned int)text_len, cc_bits);

    /* Data bytes */
    for (int i = 0; i < text_len; i++)
        bb_append(&bb, (uint8_t)text[i], 8);

    /*
     * Number of data codewords, derived from the ECC block structure
     * (NOT the character capacity, which is smaller by the
     * mode + character count header).
     */
    int total_data_cw = ei->blocks_g1 * ei->data_g1 + ei->blocks_g2 * ei->data_g2;
    int total_bits    = total_data_cw * 8;
    if (bb.bit_len > total_bits) return -1;

    /* Terminator: up to 4 zero bits */
    int remaining = total_bits - bb.bit_len;
    int term_bits = remaining < 4 ? remaining : 4;
    bb_append(&bb, 0, term_bits);

    /* Pad to byte boundary */
    while (bb.bit_len % 8 != 0)
        bb_append(&bb, 0, 1);

    /* Pad with 0xEC/0x11 alternating to fill data codewords */
    int pad_byte = 0;
    while (bb.bit_len < total_bits) {
        bb_append(&bb, pad_byte ? 0x11 : 0xEC, 8);
        pad_byte ^= 1;
    }

    int cw_count = bb.bit_len / 8;
    if (cw_count > out_size) return -1;
    memcpy(out, bb.data, (size_t)cw_count);
    return cw_count;
}

/*
 * Build the full interleaved codeword sequence (data + ECC) for the given
 * block structure. Returns total number of codewords written.
 */
static int build_codewords(const uint8_t *data_cw, int data_cw_count,
                             const EccInfo *ei, uint8_t *out, int out_size)
{
    (void)data_cw_count; /* block sizes are determined from the ECC table */
    int total_blocks = ei->blocks_g1 + ei->blocks_g2;
    if (total_blocks == 0 || total_blocks > 16) return -1;

    /* Block data and ECC. Maxima over the v1-11 tables: 11 blocks (H v11),
     * 116 data codewords per block (L v9), 30 EC codewords per block. */
    uint8_t block_data[16][128];
    uint8_t block_ecc[16][64];
    int     block_len[16];
    if (ei->data_g1 > 128 || ei->data_g2 > 128 || ei->ecc_per_block > 64) {
        return -1;
    }

    int pos = 0;
    for (int i = 0; i < total_blocks; i++) {
        int is_g2 = (i >= ei->blocks_g1);
        int dlen  = is_g2 ? ei->data_g2 : ei->data_g1;
        block_len[i] = dlen;
        memcpy(block_data[i], data_cw + pos, (size_t)dlen);
        pos += dlen;
        rs_generate(block_data[i], dlen, block_ecc[i], ei->ecc_per_block);
    }

    /* Interleave data codewords */
    int max_data = ei->data_g2 > ei->data_g1 ? ei->data_g2 : ei->data_g1;
    int out_pos = 0;
    for (int cw = 0; cw < max_data; cw++) {
        for (int blk = 0; blk < total_blocks; blk++) {
            if (cw < block_len[blk]) {
                if (out_pos >= out_size) return -1;
                out[out_pos++] = block_data[blk][cw];
            }
        }
    }

    /* Interleave ECC codewords */
    for (int cw = 0; cw < ei->ecc_per_block; cw++) {
        for (int blk = 0; blk < total_blocks; blk++) {
            if (out_pos >= out_size) return -1;
            out[out_pos++] = block_ecc[blk][cw];
        }
    }

    return out_pos;
}

/* ── Matrix Helpers ─────────────────────────────────────────────────────── */

/* Per-module flags (to track reserved/function modules) */
#define MOD_DATA     0
#define MOD_RESERVED 1

typedef struct {
    uint8_t module[MAX_MODULES][MAX_MODULES];  /* 0 = white, 1 = black */
    uint8_t reserved[MAX_MODULES][MAX_MODULES];/* 1 = function module */
    int     size;
} Matrix;

static void mat_init(Matrix *m, int version)
{
    m->size = 17 + 4 * version;
    memset(m->module,   0, sizeof(m->module));
    memset(m->reserved, 0, sizeof(m->reserved));
}

static void mat_set(Matrix *m, int row, int col, int black)
{
    if (row < 0 || col < 0 || row >= m->size || col >= m->size) return;
    m->module[row][col]   = black ? 1 : 0;
    m->reserved[row][col] = 1;
}

static void mat_set_data(Matrix *m, int row, int col, int black)
{
    if (row < 0 || col < 0 || row >= m->size || col >= m->size) return;
    m->module[row][col] = black ? 1 : 0;
}

/* Draw a filled rectangle */
static void mat_fill_rect(Matrix *m, int row, int col, int rows, int cols, int black)
{
    for (int r = row; r < row + rows; r++)
        for (int c = col; c < col + cols; c++)
            mat_set(m, r, c, black);
}

/* ── Pattern Placement ──────────────────────────────────────────────────── */

/* 7x7 finder pattern (black border, white ring, black center) */
static void place_finder(Matrix *m, int top, int left)
{
    /* Outer 7x7 black border */
    mat_fill_rect(m, top, left, 7, 7, 1);
    /* Inner 5x5 white */
    mat_fill_rect(m, top + 1, left + 1, 5, 5, 0);
    /* Inner 3x3 black */
    mat_fill_rect(m, top + 2, left + 2, 3, 3, 1);
}

/* Separator (white row/column adjacent to finder) */
static void place_separators(Matrix *m)
{
    int n = m->size;
    /* Top-left finder separator */
    for (int i = 0; i <= 7; i++) mat_set(m, 7, i, 0);
    for (int i = 0; i <= 7; i++) mat_set(m, i, 7, 0);
    /* Top-right finder separator */
    for (int i = 0; i <= 7; i++) mat_set(m, 7, n - 1 - i, 0);
    for (int i = 0; i <= 7; i++) mat_set(m, i, n - 8, 0);
    /* Bottom-left finder separator */
    for (int i = 0; i <= 7; i++) mat_set(m, n - 8, i, 0);
    for (int i = 0; i <= 7; i++) mat_set(m, n - 1 - i, 7, 0);
}

/* Timing patterns (alternating black/white row 6 and col 6) */
static void place_timing(Matrix *m)
{
    int n = m->size;
    for (int i = 8; i < n - 8; i++) {
        mat_set(m, 6, i, (i % 2 == 0) ? 1 : 0);
        mat_set(m, i, 6, (i % 2 == 0) ? 1 : 0);
    }
}

/* 5x5 alignment pattern centered at (row, col) */
static void place_alignment(Matrix *m, int row, int col)
{
    mat_fill_rect(m, row - 2, col - 2, 5, 5, 1);
    mat_fill_rect(m, row - 1, col - 1, 3, 3, 0);
    mat_set(m, row, col, 1);
}

static void place_alignment_patterns(Matrix *m, int version)
{
    const int *centers = ALIGN_CENTERS[version];
    /* Collect centers into array */
    int cc[7], nc = 0;
    for (int i = 0; centers[i] != -1 && i < 7; i++)
        cc[nc++] = centers[i];

    for (int i = 0; i < nc; i++) {
        for (int j = 0; j < nc; j++) {
            int r = cc[i], c = cc[j];
            /* Skip positions that overlap finder patterns */
            if ((r <= 8 && c <= 8) ||
                (r <= 8 && c >= m->size - 8) ||
                (r >= m->size - 8 && c <= 8))
                continue;
            place_alignment(m, r, c);
        }
    }
}

/*
 * Dark module at (4*version + 9, 8) — always black.
 * Also reserve the format info areas.
 */
static void reserve_format_areas(Matrix *m)
{
    int n = m->size;
    /* Format info around top-left finder */
    for (int i = 0; i <= 8; i++) {
        m->reserved[8][i] = 1;
        m->reserved[i][8] = 1;
    }
    /* Format info around top-right finder */
    for (int i = n - 8; i < n; i++)
        m->reserved[8][i] = 1;
    /* Format info around bottom-left finder */
    for (int i = n - 7; i < n; i++)
        m->reserved[i][8] = 1;
    /* Dark module */
    m->reserved[n - 8][8] = 1;
    m->module[n - 8][8]   = 1; /* always black */
}

/*
 * 18-bit version information value for versions >= 7: the 6-bit version
 * number followed by a 12-bit BCH(18,6) remainder using the generator
 * polynomial G(x) = x^12 + x^11 + x^10 + x^9 + x^8 + x^5 + x^2 + 1 (0x1F25).
 * ISO/IEC 18004:2015 Annex D; e.g. version 7 -> 0x07C94, version 8 -> 0x085BC.
 */
static uint32_t version_info_bits(int version)
{
    if (version < 7) return 0;
    uint32_t rem = (uint32_t)version << 12;
    for (int i = 17; i >= 12; i--) {
        if (rem & (1U << i))
            rem ^= 0x1F25U << (i - 12);
    }
    return ((uint32_t)version << 12) | rem;
}

/*
 * Place the two version information blocks (versions >= 7):
 *   - bottom-left: 3 rows x 6 cols, rows n-11..n-9, cols 0..5
 *   - top-right:   6 rows x 3 cols, rows 0..5, cols n-11..n-9
 * Bit i (LSB first) goes to (row i/3, col n-11 + i%3) in the top-right
 * block and to its transpose in the bottom-left block. Version info areas
 * are function modules: reserved, and never masked.
 */
static void place_version_info(Matrix *m, int version)
{
    if (version < 7) return;
    uint32_t bits = version_info_bits(version);
    int n = m->size;
    for (int i = 0; i < 18; i++) {
        int v = (int)((bits >> i) & 1U);
        int r = i / 3;
        int c = n - 11 + i % 3;
        mat_set(m, r, c, v); /* top-right block */
        mat_set(m, c, r, v); /* bottom-left block (transposed) */
    }
}

/*
 * Format information string: 2 ECC level indicator bits, 3 mask bits,
 * then a BCH(15,5) remainder, XORed with the fixed mask.
 * Level indicator bits (ISO/IEC 18004 Table 12): L=01 M=00 Q=11 H=10 —
 * cross-checked against segno's level constants at dev time.
 * BCH generator: x^10 + x^8 + x^5 + x^4 + x^2 + x + 1 (0x537)
 * XOR mask: 101010000010010 = 0x5412
 */
static uint16_t format_string(QrEcLevel level, int mask)
{
    static const uint32_t level_bits[4] = {
        [QR_EC_L] = 0x1, [QR_EC_M] = 0x0, [QR_EC_Q] = 0x3, [QR_EC_H] = 0x2,
    };
    uint32_t data = (level_bits[level] << 3) | (uint32_t)mask; /* 5 bits */
    /* Calculate BCH(15,5) remainder */
    uint32_t g = 0x537; /* generator: x^10+x^8+x^5+x^4+x^2+x+1 */
    uint32_t rem = data << 10;
    for (int i = 14; i >= 10; i--) {
        if (rem & (1U << i))
            rem ^= g << (i - 10);
    }
    uint16_t fmt = (uint16_t)((data << 10) | (rem & 0x3FF));
    fmt ^= 0x5412; /* XOR mask */
    return fmt;
}

static void place_format_info(Matrix *m, QrEcLevel level, int mask)
{
    uint16_t fmt = format_string(level, mask);
    int n = m->size;

    /*
     * Format string bit positions per ISO/IEC 18004 Figure 25 (bit 0 = LSB):
     * Copy 1, around the top-left finder:
     *   col 8, rows 0..5        -> bits 0..5   (skip timing row 6)
     *   (7,8), (8,8), (8,7)     -> bits 6, 7, 8
     *   row 8, cols 5..0        -> bits 9..14  (skip timing col 6)
     * Copy 2:
     *   row 8, cols n-1..n-8    -> bits 0..7   (top-right)
     *   col 8, rows n-7..n-1    -> bits 8..14  (bottom-left)
     */

    /* Copy 1: vertical strip in col 8, top to bottom */
    for (int i = 0; i <= 5; i++)
        m->module[i][8] = (uint8_t)((fmt >> i) & 1);
    m->module[7][8] = (uint8_t)((fmt >> 6) & 1);
    m->module[8][8] = (uint8_t)((fmt >> 7) & 1);
    m->module[8][7] = (uint8_t)((fmt >> 8) & 1);
    /* Copy 1: horizontal strip in row 8, right to left */
    for (int i = 9; i <= 14; i++)
        m->module[8][14 - i] = (uint8_t)((fmt >> i) & 1);

    /* Copy 2: top-right (row 8, cols n-1..n-8, bits 0..7) */
    for (int i = 0; i <= 7; i++)
        m->module[8][n - 1 - i] = (uint8_t)((fmt >> i) & 1);

    /* Copy 2: bottom-left (col 8, rows n-7..n-1, bits 8..14) */
    for (int i = 8; i <= 14; i++)
        m->module[n - 15 + i][8] = (uint8_t)((fmt >> i) & 1);
}

/* ── Data Placement (zigzag) ────────────────────────────────────────────── */

static void place_data_bits(Matrix *m, const uint8_t *cw, int cw_count)
{
    int n = m->size;
    int bit_idx = 0;
    int total_bits = cw_count * 8;

    /*
     * Traverse columns in pairs from right to left.
     * Column 6 is the timing column — skip it (use col 5 instead in that pair).
     */
    int col = n - 1;
    int going_up = 1;

    while (col >= 0 && bit_idx < total_bits) {
        /* The vertical timing pattern occupies column 6: every column pair
         * to its left is shifted one to the left, so the pair that would
         * start at column 6 becomes (5, 4). */
        int c1 = col;
        int c0 = col - 1;
        if (c1 == 6) {
            col -= 1;
            c1 = col;
            c0 = col - 1;
        }

        /* Iterate rows in current direction */
        int row_start = going_up ? (n - 1) : 0;
        int row_end   = going_up ? -1 : n;
        int row_step  = going_up ? -1 : 1;

        for (int row = row_start; row != row_end && bit_idx < total_bits; row += row_step) {
            /* Right column of pair */
            if (c1 >= 0 && !m->reserved[row][c1]) {
                int bit_val = (cw[bit_idx / 8] >> (7 - (bit_idx % 8))) & 1;
                mat_set_data(m, row, c1, bit_val);
                bit_idx++;
            }
            /* Left column of pair */
            if (c0 >= 0 && !m->reserved[row][c0] && bit_idx < total_bits) {
                int bit_val = (cw[bit_idx / 8] >> (7 - (bit_idx % 8))) & 1;
                mat_set_data(m, row, c0, bit_val);
                bit_idx++;
            }
        }

        going_up ^= 1;
        col -= 2;
    }
}

/* ── Masking ────────────────────────────────────────────────────────────── */

static int mask_condition(int pattern, int row, int col)
{
    switch (pattern) {
    case 0: return (row + col) % 2 == 0;
    case 1: return row % 2 == 0;
    case 2: return col % 3 == 0;
    case 3: return (row + col) % 3 == 0;
    case 4: return (row / 2 + col / 3) % 2 == 0;
    case 5: return (row * col) % 2 + (row * col) % 3 == 0;
    case 6: return ((row * col) % 2 + (row * col) % 3) % 2 == 0;
    case 7: return ((row + col) % 2 + (row * col) % 3) % 2 == 0;
    default: return 0;
    }
}

static void apply_mask(Matrix *m, int pattern)
{
    for (int row = 0; row < m->size; row++) {
        for (int col = 0; col < m->size; col++) {
            if (!m->reserved[row][col] && mask_condition(pattern, row, col))
                m->module[row][col] ^= 1;
        }
    }
}

/*
 * Penalty scoring per ISO/IEC 18004 section 7.8.3.
 */
static int score_penalty(const Matrix *m)
{
    int n = m->size;
    int penalty = 0;

    /* Rule 1: runs of 5+ same-color modules in a row/column */
    for (int row = 0; row < n; row++) {
        int run = 1;
        for (int col = 1; col < n; col++) {
            if (m->module[row][col] == m->module[row][col - 1]) {
                run++;
                if (run == 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else {
                run = 1;
            }
        }
    }
    for (int col = 0; col < n; col++) {
        int run = 1;
        for (int row = 1; row < n; row++) {
            if (m->module[row][col] == m->module[row - 1][col]) {
                run++;
                if (run == 5) penalty += 3;
                else if (run > 5) penalty += 1;
            } else {
                run = 1;
            }
        }
    }

    /* Rule 2: 2x2 blocks of same color */
    for (int row = 0; row < n - 1; row++) {
        for (int col = 0; col < n - 1; col++) {
            uint8_t v = m->module[row][col];
            if (v == m->module[row][col + 1] &&
                v == m->module[row + 1][col] &&
                v == m->module[row + 1][col + 1])
                penalty += 3;
        }
    }

    /*
     * Rule 3: finder-like patterns — 1:1:3:1:1 dark/light ratio (1011101)
     * with 4 light modules on either side. ISO/IEC 18004 also counts
     * occurrences whose 4-module light run falls in the quiet zone, so each
     * scan line is padded with 4 virtual light modules at both edges.
     */
    {
        static const uint8_t p1[11] = {1,0,1,1,1,0,1,0,0,0,0};
        static const uint8_t p2[11] = {0,0,0,0,1,0,1,1,1,0,1};
        uint8_t line[MAX_MODULES + 8];
        memset(line, 0, sizeof(line)); /* the 4-module pads stay light */
        for (int row = 0; row < n; row++) {
            for (int col = 0; col < n; col++)
                line[4 + col] = m->module[row][col];
            for (int col = 0; col + 11 <= n + 8; col++) {
                int m1 = 1, m2 = 1;
                for (int k = 0; k < 11; k++) {
                    if (line[col + k] != p1[k]) m1 = 0;
                    if (line[col + k] != p2[k]) m2 = 0;
                }
                if (m1) penalty += 40;
                if (m2) penalty += 40;
            }
        }
        for (int col = 0; col < n; col++) {
            for (int row = 0; row < n; row++)
                line[4 + row] = m->module[row][col];
            for (int row = 0; row + 11 <= n + 8; row++) {
                int m1 = 1, m2 = 1;
                for (int k = 0; k < 11; k++) {
                    if (line[row + k] != p1[k]) m1 = 0;
                    if (line[row + k] != p2[k]) m2 = 0;
                }
                if (m1) penalty += 40;
                if (m2) penalty += 40;
            }
        }
    }

    /* Rule 4: proportion of dark modules */
    int dark = 0;
    for (int row = 0; row < n; row++)
        for (int col = 0; col < n; col++)
            if (m->module[row][col]) dark++;
    int total = n * n;
    int pct = dark * 100 / total;
    /* Nearest multiples of 5, above and below */
    int prev5 = (pct / 5) * 5;
    int next5 = prev5 + 5;
    int d1 = abs(prev5 - 50) / 5;
    int d2 = abs(next5 - 50) / 5;
    penalty += (d1 < d2 ? d1 : d2) * 10;

    return penalty;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

QrCode *qr_encode(const char *text)
{
    return qr_encode_level(text, QR_EC_M);
}

QrCode *qr_encode_level(const char *text, QrEcLevel level)
{
    if (!text || (int)level < 0 || (int)level > 3) return NULL;
    gf_init();

    int text_len = (int)strlen(text);

    /* Find smallest version that fits (byte-mode character capacity at the
     * requested level). The terminator may be truncated when the data fills
     * the symbol, so the derived capacity — not "header + data + 4 bits" —
     * is the right check. */
    int version = -1;
    for (int v = 1; v <= MAX_VERSION; v++) {
        if (text_len <= data_capacity(v, level)) {
            version = v;
            break;
        }
    }
    if (version < 0) return NULL; /* text too long */
    const EccInfo *ei = &ECC_INFO[level][version];

    /* Encode data codewords (max over the tables: 324, L v11) */
    uint8_t data_cw[512];
    int data_cw_count = encode_data(text, text_len, version, ei,
                                    data_cw, (int)sizeof(data_cw));
    if (data_cw_count < 0) return NULL;

    /* Build interleaved codeword sequence */
    uint8_t all_cw[1024];
    int all_cw_count = build_codewords(data_cw, data_cw_count, ei,
                                        all_cw, (int)sizeof(all_cw));
    if (all_cw_count < 0) return NULL;

    /* Build the base matrix (without data or mask) */
    Matrix m;
    mat_init(&m, version);

    /* Finder patterns at three corners */
    place_finder(&m, 0, 0);                       /* top-left */
    place_finder(&m, 0, m.size - 7);              /* top-right */
    place_finder(&m, m.size - 7, 0);              /* bottom-left */
    place_separators(&m);
    place_timing(&m);
    place_alignment_patterns(&m, version);
    reserve_format_areas(&m);
    place_version_info(&m, version); /* versions >= 7; also reserves the areas */

    /* Place data bits */
    place_data_bits(&m, all_cw, all_cw_count);

    /* Try all 8 mask patterns, pick the one with lowest penalty */
    int best_mask    = 0;
    int best_penalty = 0x7FFFFFFF;

    for (int mask = 0; mask < 8; mask++) {
        /* Apply mask to a temporary copy */
        Matrix tmp = m;
        apply_mask(&tmp, mask);
        place_format_info(&tmp, level, mask);
        int p = score_penalty(&tmp);
        if (p < best_penalty) {
            best_penalty = p;
            best_mask    = mask;
        }
    }

    /* Apply best mask permanently */
    apply_mask(&m, best_mask);
    place_format_info(&m, level, best_mask);

    /* Build output */
    int sz = m.size;
    QrCode *qr = malloc(sizeof(QrCode));
    if (!qr) return NULL;
    qr->modules = malloc((size_t)(sz * sz));
    if (!qr->modules) { free(qr); return NULL; }
    qr->size = sz;
    for (int row = 0; row < sz; row++)
        memcpy(qr->modules + row * sz, m.module[row], (size_t)sz);

    return qr;
}

void qr_free(QrCode *qr)
{
    if (!qr) return;
    free(qr->modules);
    free(qr);
}

/* ── Test Introspection Hooks ───────────────────────────────────────────── */

int qr_ecc_block_info(int version, QrEcLevel level,
                      int *total_cw, int *ecc_per_block,
                      int *blocks_g1, int *data_g1,
                      int *blocks_g2, int *data_g2)
{
    if (version < 1 || version > MAX_VERSION ||
        (int)level < 0 || (int)level > 3) return -1;
    const EccInfo *ei = &ECC_INFO[level][version];
    if (total_cw)      *total_cw      = ei->total_cw;
    if (ecc_per_block) *ecc_per_block = ei->ecc_per_block;
    if (blocks_g1)     *blocks_g1     = ei->blocks_g1;
    if (data_g1)       *data_g1       = ei->data_g1;
    if (blocks_g2)     *blocks_g2     = ei->blocks_g2;
    if (data_g2)       *data_g2       = ei->data_g2;
    return 0;
}

void qr_rs_ecc(const uint8_t *data, int data_len, uint8_t *ecc, int num_ecc)
{
    gf_init();
    rs_generate(data, data_len, ecc, num_ecc);
}

uint32_t qr_version_info_bits(int version)
{
    return version_info_bits(version);
}

int qr_max_version(void)
{
    return MAX_VERSION;
}
