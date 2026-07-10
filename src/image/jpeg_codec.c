#include "jpeg_codec.h"
#include <math.h>
#include <string.h>

// Baseline JPEG codec, written from scratch against ITU-T T.81. The decoder
// handles baseline sequential Huffman (SOF0, plus SOF1 which decodes
// identically for 8-bit samples): grayscale and 3-component YCbCr with
// sampling factors 1..2 per axis (4:4:4, 4:2:2, 4:2:0, 4:4:0), restart
// intervals, and APPn/COM tolerance. Everything else — progressive (SOF2),
// lossless (SOF3), hierarchical (SOF5-7/DHP), arithmetic coding (SOF9-11,
// DAC), 12-bit precision, CMYK/YCCK — is rejected so callers can pass the
// original stream through untouched. All allocation goes through the caller's
// arena; the input is only ever read inside [data, data+len).
//
// The DCT/IDCT is a direct separable float transform (8x8 cosine matrix,
// two 8x8x8 passes per block). It is exact to well under 1 LSB, deterministic
// (fixed operation order, no fast-math), and fast enough that a 4000x5000
// encode stays well under a second at -O2. Chroma upsampling replicates
// libjpeg's "fancy" triangular filter so our output matches what
// libjpeg-turbo-based viewers (and the PIL oracle in the tests) produce.

#define JC_MAX_DIMENSION 65535
// Cap total pixels so a tiny malformed SOF cannot demand a multi-gigabyte
// raster (same defence as TSPDF_PNG_MAX_DIMENSION for PNG). 100M pixels is
// a 600dpi A2 scan; nothing legitimate in a PDF exceeds it.
#define JC_MAX_PIXELS 100000000

// Fixed-point color-convert constants, same scaling as libjpeg (16 fraction
// bits, round-half-up) so decoded pixels match the reference within ±1.
#define JC_FIX(x) ((int32_t)((x) * 65536.0 + 0.5))

// Zigzag order: jc_zigzag[k] = natural (row-major) index of zigzag position k.
static const uint8_t jc_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static uint16_t jc_read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int jc_clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

// 8x8 DCT basis matrix A[u][x] = c(u)/2 * cos((2x+1) u pi / 16), so that
// FDCT is A f A^T and IDCT is A^T F A. Filled once per encode/decode call
// (64 cos() calls, negligible; keeps the codec free of mutable globals).
static void jc_dct_matrix(float a[8][8]) {
    for (int u = 0; u < 8; u++) {
        double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
        for (int x = 0; x < 8; x++) {
            a[u][x] = (float)(0.5 * cu * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        }
    }
}

// Inverse DCT of one dequantized block (natural order) with level shift,
// clamped into an 8-row window of the component plane at `stride`.
static void jc_idct_block(const float a[8][8], const float *coef,
                          uint8_t *dst, int stride) {
    float tmp[64];
    // tmp[u][y] = sum_v coef[u][v] * A[v][y]
    for (int u = 0; u < 8; u++) {
        for (int y = 0; y < 8; y++) {
            float s = 0.0f;
            for (int v = 0; v < 8; v++) s += coef[u * 8 + v] * a[v][y];
            tmp[u * 8 + y] = s;
        }
    }
    // f[x][y] = sum_u A[u][x] * tmp[u][y]
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float s = 0.0f;
            for (int u = 0; u < 8; u++) s += a[u][x] * tmp[u * 8 + y];
            int v = (int)(s + 128.5f);
            dst[x * stride + y] = (uint8_t)jc_clamp255(v);
        }
    }
}

// ============================================================
// Decoder
// ============================================================

typedef struct {
    uint8_t bits[17];    // count of codes per length 1..16
    uint8_t values[256];
    int32_t mincode[17];
    int32_t maxcode[17]; // -1 where no codes of that length exist
    int32_t valptr[17];
    uint8_t fast_len[256]; // codes resolvable in <= 8 bits; 0 = slow path
    uint8_t fast_val[256];
    bool defined;
} JHuff;

static bool jc_huff_build(JHuff *h) {
    int32_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        int n = h->bits[l];
        // A valid prefix code never overflows l bits.
        if (code + n > (1 << l)) return false;
        code += n;
        k += n;
        h->maxcode[l] = (n > 0) ? code - 1 : -1;
        code <<= 1;
    }
    memset(h->fast_len, 0, sizeof(h->fast_len));
    // Fast table: every code of length <= 8 fills the 8-bit prefixes it owns.
    k = 0;
    code = 0;
    for (int l = 1; l <= 8; l++) {
        for (int i = 0; i < h->bits[l]; i++, k++, code++) {
            int lo = code << (8 - l);
            for (int j = 0; j < (1 << (8 - l)); j++) {
                h->fast_len[lo + j] = (uint8_t)l;
                h->fast_val[lo + j] = h->values[k];
            }
        }
        code <<= 1;
    }
    h->defined = true;
    return true;
}

// Entropy-coded-segment bit reader, MSB first. 0xFF 0x00 stuffing is removed;
// any other 0xFF-marker stops the fill (the byte is left unconsumed so the
// scan loop can inspect RSTn/EOI). Requesting bits past the available data
// sets err — the caller aborts, so truncated streams fail instead of looping.
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    uint32_t buf; // MSB-aligned
    int cnt;
    bool at_marker;
    bool err;
} JBitReader;

static void jc_bits_fill(JBitReader *b) {
    while (b->cnt <= 24 && !b->at_marker) {
        if (b->pos >= b->len) { b->at_marker = true; return; }
        uint8_t byte = b->p[b->pos];
        if (byte == 0xFF) {
            if (b->pos + 1 >= b->len || b->p[b->pos + 1] != 0x00) {
                b->at_marker = true;
                return;
            }
            b->pos += 2; // stuffed 0xFF
        } else {
            b->pos++;
        }
        b->buf |= (uint32_t)byte << (24 - b->cnt);
        b->cnt += 8;
    }
}

static int jc_bits_get(JBitReader *b, int n) {
    if (n <= 0) return 0;
    if (b->cnt < n) {
        jc_bits_fill(b);
        if (b->cnt < n) { b->err = true; return 0; }
    }
    int v = (int)(b->buf >> (32 - n));
    b->buf <<= n;
    b->cnt -= n;
    return v;
}

static int jc_huff_decode(JBitReader *b, const JHuff *h) {
    if (b->cnt < 16) jc_bits_fill(b);
    if (b->cnt >= 8) {
        int idx = (int)(b->buf >> 24);
        int l = h->fast_len[idx];
        if (l) {
            b->buf <<= l;
            b->cnt -= l;
            return h->fast_val[idx];
        }
    }
    int32_t code = jc_bits_get(b, 1);
    for (int l = 1; l <= 16; l++) {
        if (b->err) return -1;
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) {
            return h->values[h->valptr[l] + (code - h->mincode[l])];
        }
        code = (code << 1) | jc_bits_get(b, 1);
    }
    return -1; // not a valid code
}

// Receive s magnitude bits and sign-extend per F.2.2.1 (EXTEND).
static int jc_receive_extend(JBitReader *b, int s) {
    int v = jc_bits_get(b, s);
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    return v;
}

typedef struct {
    int id;
    int h, v;      // sampling factors (1..2 accepted)
    int tq;        // quantization table id
    int dc_tbl, ac_tbl;
    int bw, bh;    // plane size in blocks (MCU padded)
    uint8_t *plane;
    int dc_pred;
} JComp;

typedef struct {
    const uint8_t *data;
    size_t len;
    int width, height, ncomp;
    int hmax, vmax;
    JComp comp[3];
    uint16_t qt[4][64]; // zigzag order, as stored in DQT
    bool qt_def[4];
    JHuff hdc[4], hac[4];
    int restart_interval;
    int adobe_transform; // -1 = no Adobe APP14
    bool frame_seen;
    float dctm[8][8];
} JDec;

static bool jc_parse_dqt(JDec *d, const uint8_t *seg, size_t seglen) {
    size_t i = 0;
    while (i < seglen) {
        int pq = seg[i] >> 4, tq = seg[i] & 15;
        i++;
        if (tq > 3 || pq > 1) return false;
        size_t need = pq ? 128 : 64;
        if (seglen - i < need) return false;
        for (int k = 0; k < 64; k++) {
            uint16_t q = pq ? jc_read_be16(seg + i + 2 * k) : seg[i + k];
            if (q == 0) return false;
            d->qt[tq][k] = q;
        }
        d->qt_def[tq] = true;
        i += need;
    }
    return true;
}

static bool jc_parse_dht(JDec *d, const uint8_t *seg, size_t seglen) {
    size_t i = 0;
    while (i < seglen) {
        if (seglen - i < 17) return false;
        int tc = seg[i] >> 4, th = seg[i] & 15;
        i++;
        if (tc > 1 || th > 3) return false;
        JHuff *h = (tc == 0) ? &d->hdc[th] : &d->hac[th];
        memset(h, 0, sizeof(*h));
        int total = 0;
        for (int l = 1; l <= 16; l++) {
            h->bits[l] = seg[i + l - 1];
            total += h->bits[l];
        }
        i += 16;
        if (total > 256 || seglen - i < (size_t)total) return false;
        memcpy(h->values, seg + i, (size_t)total);
        i += (size_t)total;
        if (!jc_huff_build(h)) return false;
    }
    return true;
}

static bool jc_parse_sof(JDec *d, const uint8_t *seg, size_t seglen) {
    if (d->frame_seen || seglen < 6) return false;
    int precision = seg[0];
    d->height = jc_read_be16(seg + 1);
    d->width = jc_read_be16(seg + 3);
    d->ncomp = seg[5];
    if (precision != 8) return false; // 12-bit is not baseline
    if (d->width < 1 || d->height < 1) return false; // height 0 = DNL, reject
    if ((int64_t)d->width * d->height > JC_MAX_PIXELS) return false;
    if (d->ncomp != 1 && d->ncomp != 3) return false; // 4-comp CMYK/YCCK etc.
    if (seglen < 6 + (size_t)d->ncomp * 3) return false;
    d->hmax = 1;
    d->vmax = 1;
    for (int c = 0; c < d->ncomp; c++) {
        JComp *cp = &d->comp[c];
        cp->id = seg[6 + c * 3];
        cp->h = seg[7 + c * 3] >> 4;
        cp->v = seg[7 + c * 3] & 15;
        cp->tq = seg[8 + c * 3];
        if (cp->h < 1 || cp->h > 2 || cp->v < 1 || cp->v > 2) return false;
        if (cp->tq > 3) return false;
        // A single-component frame is decoded non-interleaved, where sampling
        // factors are irrelevant (T.81 A.2.2); normalize them to 1x1.
        if (d->ncomp == 1) cp->h = cp->v = 1;
        if (cp->h > d->hmax) d->hmax = cp->h;
        if (cp->v > d->vmax) d->vmax = cp->v;
        for (int p = 0; p < c; p++) {
            if (d->comp[p].id == cp->id) return false;
        }
    }
    d->frame_seen = true;
    return true;
}

// Decode one 8x8 block: DC diff + AC run-length pairs, dequantize into
// natural order, IDCT into the component plane.
static bool jc_decode_block(JDec *d, JBitReader *b, JComp *cp,
                            uint8_t *dst, int stride) {
    const JHuff *hdc = &d->hdc[cp->dc_tbl];
    const JHuff *hac = &d->hac[cp->ac_tbl];
    const uint16_t *qt = d->qt[cp->tq];
    float coef[64];
    memset(coef, 0, sizeof(coef));

    int t = jc_huff_decode(b, hdc);
    if (t < 0 || t > 15) return false;
    int diff = (t > 0) ? jc_receive_extend(b, t) : 0;
    // Clamp the predictor: a corrupt stream could otherwise walk it past
    // INT_MAX (UB). Decoded pixels are clamped anyway.
    if (cp->dc_pred > (1 << 26) || cp->dc_pred < -(1 << 26)) return false;
    cp->dc_pred += diff;
    coef[0] = (float)cp->dc_pred * (float)qt[0];

    int k = 1;
    while (k < 64) {
        int rs = jc_huff_decode(b, hac);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r == 15) { k += 16; continue; } // ZRL
            break; // EOB
        }
        k += r;
        if (k > 63) return false;
        int v = jc_receive_extend(b, s);
        coef[jc_zigzag[k]] = (float)v * (float)qt[k];
        k++;
    }
    if (b->err) return false;
    jc_idct_block(d->dctm, coef, dst, stride);
    return true;
}

static bool jc_decode_scan(JDec *d, TspdfArena *arena, size_t sos_pos,
                           size_t *end_pos) {
    const uint8_t *seg = d->data + sos_pos;
    size_t avail = d->len - sos_pos;
    if (avail < 3) return false;
    size_t seglen = jc_read_be16(seg);
    if (seglen < 2 || seglen > avail) return false;
    int ns = seg[2];
    // Only a single interleaved scan covering every frame component is
    // supported (what every baseline encoder emits). Multi-scan baseline
    // files are rejected like progressive ones.
    if (ns != d->ncomp || seglen < 6 + (size_t)ns * 2) return false;
    for (int i = 0; i < ns; i++) {
        int cid = seg[3 + i * 2];
        int tbl = seg[4 + i * 2];
        JComp *cp = NULL;
        for (int c = 0; c < d->ncomp; c++) {
            if (d->comp[c].id == cid) { cp = &d->comp[c]; break; }
        }
        if (!cp) return false;
        cp->dc_tbl = tbl >> 4;
        cp->ac_tbl = tbl & 15;
        if (cp->dc_tbl > 3 || cp->ac_tbl > 3) return false;
        if (!d->hdc[cp->dc_tbl].defined || !d->hac[cp->ac_tbl].defined) return false;
        if (!d->qt_def[cp->tq]) return false;
    }
    // Spectral selection / successive approximation must be the baseline
    // 0..63, 0:0 — anything else is a progressive scan header.
    const uint8_t *ss = seg + 3 + (size_t)ns * 2;
    if (ss[0] != 0 || ss[1] != 63 || ss[2] != 0) return false;

    int mcux = (d->width + 8 * d->hmax - 1) / (8 * d->hmax);
    int mcuy = (d->height + 8 * d->vmax - 1) / (8 * d->vmax);
    int blocks_per_mcu = 0;
    for (int c = 0; c < d->ncomp; c++) {
        JComp *cp = &d->comp[c];
        cp->bw = mcux * cp->h;
        cp->bh = mcuy * cp->v;
        cp->dc_pred = 0;
        blocks_per_mcu += cp->h * cp->v;
        size_t plane_size = (size_t)cp->bw * 8 * (size_t)cp->bh * 8;
        cp->plane = (uint8_t *)tspdf_arena_alloc(arena, plane_size);
        if (!cp->plane) return false;
    }
    if (blocks_per_mcu > 10) return false; // baseline MCU limit (B.2.3)

    JBitReader b = {0};
    b.p = d->data;
    b.len = d->len;
    b.pos = sos_pos + seglen;

    int rst_next = 0;
    long mcus_done = 0;
    for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
            if (d->restart_interval && mcus_done > 0 &&
                mcus_done % d->restart_interval == 0) {
                // Byte-align, drop pad bits, and consume the RSTn marker.
                b.buf = 0;
                b.cnt = 0;
                while (b.pos + 1 < b.len && b.p[b.pos] == 0xFF &&
                       b.p[b.pos + 1] == 0xFF) {
                    b.pos++; // fill bytes before the marker
                }
                if (b.pos + 2 > b.len || b.p[b.pos] != 0xFF ||
                    b.p[b.pos + 1] != (uint8_t)(0xD0 + rst_next)) {
                    return false;
                }
                b.pos += 2;
                b.at_marker = false;
                rst_next = (rst_next + 1) & 7;
                for (int c = 0; c < d->ncomp; c++) d->comp[c].dc_pred = 0;
            }
            for (int c = 0; c < d->ncomp; c++) {
                JComp *cp = &d->comp[c];
                int stride = cp->bw * 8;
                for (int by = 0; by < cp->v; by++) {
                    for (int bx = 0; bx < cp->h; bx++) {
                        uint8_t *dst = cp->plane +
                            ((size_t)(my * cp->v + by) * 8) * (size_t)stride +
                            (size_t)(mx * cp->h + bx) * 8;
                        if (!jc_decode_block(d, &b, cp, dst, stride)) return false;
                    }
                }
            }
            mcus_done++;
        }
    }
    *end_pos = b.pos;
    return true;
}

// --- Chroma upsampling ---
//
// These replicate libjpeg's "fancy" (triangular) upsampler bit for bit, so a
// stream decoded here matches libjpeg-turbo/PIL within the IDCT's ±1: for a
// x2 axis every output sample is (3*nearer + further) with round-to-even-ish
// bias constants, degenerating to plain replication at the edges. sw/sh are
// the significant (non-padding) source dimensions; dst is W x H.

static void jc_upsample_h2v1_row(const uint8_t *in, int sw, uint8_t *out) {
    if (sw == 1) {
        out[0] = in[0];
        out[1] = in[0];
        return;
    }
    out[0] = in[0];
    out[1] = (uint8_t)((in[0] * 3 + in[1] + 2) >> 2);
    for (int i = 1; i < sw - 1; i++) {
        int v = in[i] * 3;
        out[2 * i] = (uint8_t)((v + in[i - 1] + 1) >> 2);
        out[2 * i + 1] = (uint8_t)((v + in[i + 1] + 2) >> 2);
    }
    out[2 * (sw - 1)] = (uint8_t)((in[sw - 1] * 3 + in[sw - 2] + 1) >> 2);
    out[2 * sw - 1] = in[sw - 1];
}

// One output row of h2v2 fancy upsampling. colsum[i] = 3*nearer + further
// (12-bit vertical pass); this row does the horizontal pass with /16 rounding.
static void jc_upsample_h2v2_row(const int *colsum, int sw, uint8_t *out) {
    if (sw == 1) {
        out[0] = (uint8_t)((colsum[0] * 4 + 8) >> 4);
        out[1] = (uint8_t)((colsum[0] * 4 + 7) >> 4);
        return;
    }
    out[0] = (uint8_t)((colsum[0] * 4 + 8) >> 4);
    out[1] = (uint8_t)((colsum[0] * 3 + colsum[1] + 7) >> 4);
    for (int i = 1; i < sw - 1; i++) {
        int v = colsum[i] * 3;
        out[2 * i] = (uint8_t)((v + colsum[i - 1] + 8) >> 4);
        out[2 * i + 1] = (uint8_t)((v + colsum[i + 1] + 7) >> 4);
    }
    out[2 * (sw - 1)] = (uint8_t)((colsum[sw - 1] * 3 + colsum[sw - 2] + 8) >> 4);
    out[2 * sw - 1] = (uint8_t)((colsum[sw - 1] * 4 + 7) >> 4);
}

// Upsample component cp to a full W x H plane. fx/fy are the per-axis
// factors (1 or 2).
static bool jc_upsample(const JDec *d, const JComp *cp, TspdfArena *arena,
                        uint8_t *dst) {
    int W = d->width, H = d->height;
    int fx = d->hmax / cp->h, fy = d->vmax / cp->v;
    int sw = (W * cp->h + d->hmax - 1) / d->hmax;  // significant source cols
    int sh = (H * cp->v + d->vmax - 1) / d->vmax;  // significant source rows
    int stride = cp->bw * 8;

    if (fx == 1 && fy == 1) {
        for (int y = 0; y < H; y++) {
            memcpy(dst + (size_t)y * W, cp->plane + (size_t)y * stride, (size_t)W);
        }
        return true;
    }
    if (fx == 2 && fy == 1) {
        uint8_t *row = (uint8_t *)tspdf_arena_alloc(arena, (size_t)sw * 2);
        if (!row) return false;
        for (int y = 0; y < H; y++) {
            jc_upsample_h2v1_row(cp->plane + (size_t)y * stride, sw, row);
            memcpy(dst + (size_t)y * W, row, (size_t)W);
        }
        return true;
    }
    if (fx == 1 && fy == 2) {
        // Vertical-only triangular filter (libjpeg h1v2_fancy_upsample).
        for (int y = 0; y < H; y++) {
            int r = y >> 1;
            int other = (y & 1) ? r + 1 : r - 1;
            if (other < 0) other = 0;
            if (other > sh - 1) other = sh - 1;
            int bias = (y & 1) ? 2 : 1;
            const uint8_t *r0 = cp->plane + (size_t)r * stride;
            const uint8_t *r1 = cp->plane + (size_t)other * stride;
            uint8_t *o = dst + (size_t)y * W;
            for (int x = 0; x < W; x++) {
                o[x] = (uint8_t)((r0[x] * 3 + r1[x] + bias) >> 2);
            }
        }
        return true;
    }
    // fx == 2 && fy == 2
    int *colsum = (int *)tspdf_arena_alloc(arena, (size_t)sw * sizeof(int));
    uint8_t *row = (uint8_t *)tspdf_arena_alloc(arena, (size_t)sw * 2);
    if (!colsum || !row) return false;
    for (int y = 0; y < H; y++) {
        int r = y >> 1;
        int other = (y & 1) ? r + 1 : r - 1;
        if (other < 0) other = 0;
        if (other > sh - 1) other = sh - 1;
        const uint8_t *r0 = cp->plane + (size_t)r * stride;
        const uint8_t *r1 = cp->plane + (size_t)other * stride;
        for (int x = 0; x < sw; x++) colsum[x] = r0[x] * 3 + r1[x];
        jc_upsample_h2v2_row(colsum, sw, row);
        memcpy(dst + (size_t)y * W, row, (size_t)W);
    }
    return true;
}

static bool jc_build_output(JDec *d, TspdfArena *arena, TspdfRawImage *img) {
    int W = d->width, H = d->height;
    size_t npix = (size_t)W * (size_t)H;

    if (d->ncomp == 1) {
        uint8_t *out = (uint8_t *)tspdf_arena_alloc(arena, npix);
        if (!out) return false;
        int stride = d->comp[0].bw * 8;
        for (int y = 0; y < H; y++) {
            memcpy(out + (size_t)y * W, d->comp[0].plane + (size_t)y * stride,
                   (size_t)W);
        }
        img->width = W;
        img->height = H;
        img->components = 1;
        img->pixels = out;
        return true;
    }

    uint8_t *full[3];
    for (int c = 0; c < 3; c++) {
        JComp *cp = &d->comp[c];
        if (cp->h == d->hmax && cp->v == d->vmax && cp->bw * 8 == W) {
            full[c] = cp->plane; // plane rows are contiguous at width W
        } else {
            full[c] = (uint8_t *)tspdf_arena_alloc(arena, npix);
            if (!full[c]) return false;
            if (!jc_upsample(d, cp, arena, full[c])) return false;
        }
    }

    uint8_t *out = (uint8_t *)tspdf_arena_alloc(arena, npix * 3);
    if (!out) return false;
    // YCbCr -> RGB, libjpeg constants and rounding (BT.601 full range).
    const int32_t c_r_cr = JC_FIX(1.40200);
    const int32_t c_g_cb = JC_FIX(0.34414);
    const int32_t c_g_cr = JC_FIX(0.71414);
    const int32_t c_b_cb = JC_FIX(1.77200);
    for (size_t i = 0; i < npix; i++) {
        int y = full[0][i];
        int cb = full[1][i] - 128;
        int cr = full[2][i] - 128;
        out[i * 3 + 0] = (uint8_t)jc_clamp255(y + ((c_r_cr * cr + 32768) >> 16));
        out[i * 3 + 1] = (uint8_t)jc_clamp255(
            y + ((-c_g_cb * cb - c_g_cr * cr + 32768) >> 16));
        out[i * 3 + 2] = (uint8_t)jc_clamp255(y + ((c_b_cb * cb + 32768) >> 16));
    }
    img->width = W;
    img->height = H;
    img->components = 3;
    img->pixels = out;
    return true;
}

bool tspdf_jpeg_decode(const uint8_t *data, size_t len, TspdfArena *arena,
                       TspdfRawImage *img) {
    if (!data || !arena || !img || len < 4) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;

    JDec d;
    memset(&d, 0, sizeof(d));
    d.data = data;
    d.len = len;
    d.adobe_transform = -1;
    jc_dct_matrix(d.dctm);

    size_t pos = 2;
    for (;;) {
        if (pos + 2 > len) return false;
        if (data[pos] != 0xFF) return false;
        while (pos < len && data[pos] == 0xFF) pos++; // fill bytes
        if (pos >= len) return false;
        uint8_t m = data[pos++];

        if (m == 0xD9) return false; // EOI before any scan
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) return false; // stray

        // Everything else carries a 2-byte length.
        if (pos + 2 > len) return false;
        size_t seglen = jc_read_be16(data + pos);
        if (seglen < 2 || seglen > len - pos) return false;
        const uint8_t *seg = data + pos + 2;
        size_t body = seglen - 2;

        switch (m) {
        case 0xC0: // SOF0 baseline
        case 0xC1: // SOF1 extended sequential Huffman: identical for 8-bit
            if (!jc_parse_sof(&d, seg, body)) return false;
            break;
        case 0xC4:
            if (!jc_parse_dht(&d, seg, body)) return false;
            break;
        case 0xDB:
            if (!jc_parse_dqt(&d, seg, body)) return false;
            break;
        case 0xDD:
            if (body < 2) return false;
            d.restart_interval = jc_read_be16(seg);
            break;
        case 0xDA: { // SOS
            if (!d.frame_seen) return false;
            if (d.ncomp == 3 && d.adobe_transform == 2) return false; // YCCK-ish
            size_t end_pos;
            if (!jc_decode_scan(&d, arena, pos, &end_pos)) return false;
            return jc_build_output(&d, arena, img);
        }
        case 0xFE: // COM
            break;
        case 0xEE: // APP14: Adobe transform flag
            if (body >= 12 && memcmp(seg, "Adobe", 5) == 0) {
                d.adobe_transform = seg[11];
            }
            break;
        default:
            if (m >= 0xE0 && m <= 0xEF) break; // other APPn: skip
            // SOF2 (progressive), SOF3 (lossless), SOF5-7/DE (hierarchical),
            // SOF9-11 + DAC (arithmetic), and anything unrecognized.
            return false;
        }
        pos += seglen;
    }
}

// ============================================================
// Encoder
// ============================================================

// Annex K Table K.1/K.2 quantization tables, natural (row-major) order.
static const uint8_t jc_std_luma_qt[64] = {
    16,  11,  10,  16,  24,  40,  51,  61,
    12,  12,  14,  19,  26,  58,  60,  55,
    14,  13,  16,  24,  40,  57,  69,  56,
    14,  17,  22,  29,  51,  87,  80,  62,
    18,  22,  37,  56,  68, 109, 103,  77,
    24,  35,  55,  64,  81, 104, 113,  92,
    49,  64,  78,  87, 103, 121, 120, 101,
    72,  92,  95,  98, 112, 100, 103,  99
};

static const uint8_t jc_std_chroma_qt[64] = {
    17,  18,  24,  47,  99,  99,  99,  99,
    18,  21,  26,  66,  99,  99,  99,  99,
    24,  26,  56,  99,  99,  99,  99,  99,
    47,  66,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99,
    99,  99,  99,  99,  99,  99,  99,  99
};

// Annex K standard Huffman tables (K.3.3.1 / K.3.3.2).
static const uint8_t jc_dc_luma_bits[17] =
    { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t jc_dc_luma_vals[12] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t jc_dc_chroma_bits[17] =
    { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t jc_dc_chroma_vals[12] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t jc_ac_luma_bits[17] =
    { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t jc_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};
static const uint8_t jc_ac_chroma_bits[17] =
    { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t jc_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

// Encoder-side code table: code word + length per symbol.
typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} JEHuff;

static void jc_ehuff_build(JEHuff *e, const uint8_t bits[17], const uint8_t *vals) {
    memset(e->size, 0, sizeof(e->size));
    uint16_t code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++, k++) {
            e->code[vals[k]] = code;
            e->size[vals[k]] = (uint8_t)l;
            code++;
        }
        code <<= 1;
    }
}

// Growable output: a chain of arena chunks, concatenated at the end. The
// arena cannot realloc, so this keeps peak memory at ~2x the final size
// instead of reserving a worst-case entropy bound up front.
typedef struct JChunk {
    uint8_t *data;
    size_t len, cap;
    struct JChunk *next;
} JChunk;

typedef struct {
    TspdfArena *arena;
    JChunk *first, *cur;
    size_t total;
    uint32_t bitbuf; // MSB-aligned pending bits
    int bitcnt;
    bool oom;
} JOut;

static bool jc_out_grow(JOut *o) {
    size_t cap = o->cur ? o->cur->cap * 2 : 65536;
    if (cap > (1u << 24)) cap = 1u << 24;
    JChunk *c = (JChunk *)tspdf_arena_alloc(o->arena, sizeof(JChunk));
    if (!c) { o->oom = true; return false; }
    c->data = (uint8_t *)tspdf_arena_alloc(o->arena, cap);
    if (!c->data) { o->oom = true; return false; }
    c->len = 0;
    c->cap = cap;
    c->next = NULL;
    if (o->cur) o->cur->next = c; else o->first = c;
    o->cur = c;
    return true;
}

static void jc_out_byte(JOut *o, uint8_t byte) {
    if (o->oom) return;
    if (!o->cur || o->cur->len == o->cur->cap) {
        if (!jc_out_grow(o)) return;
    }
    o->cur->data[o->cur->len++] = byte;
    o->total++;
}

static void jc_out_be16(JOut *o, uint16_t v) {
    jc_out_byte(o, (uint8_t)(v >> 8));
    jc_out_byte(o, (uint8_t)(v & 0xFF));
}

static void jc_out_bytes(JOut *o, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) jc_out_byte(o, p[i]);
}

static void jc_out_bits(JOut *o, uint32_t bits, int n) {
    // n is 1..16 per call (code word and magnitude bits are emitted separately).
    if (n <= 0) return;
    o->bitbuf |= (bits << (32 - n)) >> o->bitcnt;
    o->bitcnt += n;
    while (o->bitcnt >= 8) {
        uint8_t byte = (uint8_t)(o->bitbuf >> 24);
        jc_out_byte(o, byte);
        if (byte == 0xFF) jc_out_byte(o, 0x00); // stuffing
        o->bitbuf <<= 8;
        o->bitcnt -= 8;
    }
}

static void jc_out_flush_bits(JOut *o) {
    if (o->bitcnt > 0) {
        // Pad the final byte with 1-bits (T.81 F.1.2.3).
        jc_out_bits(o, (1u << (8 - o->bitcnt)) - 1, 8 - o->bitcnt);
    }
    o->bitbuf = 0;
    o->bitcnt = 0;
}

// Magnitude category: smallest s with |v| < 2^s (v != 0 -> 1..11).
static int jc_category(int v) {
    int a = v < 0 ? -v : v;
    int s = 0;
    while (a) { s++; a >>= 1; }
    return s;
}

// Forward DCT + quantize one 8x8 block from plane (top-left at src, given
// stride), emit Huffman-coded coefficients.
static void jc_encode_block(JOut *o, const float a[8][8], const uint8_t *src,
                            int stride, const float *rquant,
                            const JEHuff *hdc, const JEHuff *hac, int *dc_pred) {
    float f[64], tmp[64];
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            f[x * 8 + y] = (float)src[x * stride + y] - 128.0f;
        }
    }
    // T[u][y] = sum_x A[u][x] f[x][y]
    for (int u = 0; u < 8; u++) {
        for (int y = 0; y < 8; y++) {
            float s = 0.0f;
            for (int x = 0; x < 8; x++) s += a[u][x] * f[x * 8 + y];
            tmp[u * 8 + y] = s;
        }
    }
    int q[64]; // natural order
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            float s = 0.0f;
            for (int y = 0; y < 8; y++) s += tmp[u * 8 + y] * a[v][y];
            float t = s * rquant[u * 8 + v];
            int qv = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
            if (qv > 2047) qv = 2047;
            if (qv < -2047) qv = -2047;
            q[u * 8 + v] = qv;
        }
    }

    int diff = q[0] - *dc_pred;
    *dc_pred = q[0];
    int s = jc_category(diff);
    jc_out_bits(o, hdc->code[s], hdc->size[s]);
    if (s) {
        int bits = diff < 0 ? diff + (1 << s) - 1 : diff;
        jc_out_bits(o, (uint32_t)bits & ((1u << s) - 1), s);
    }

    int run = 0;
    for (int k = 1; k < 64; k++) {
        int v = q[jc_zigzag[k]];
        if (v == 0) { run++; continue; }
        while (run > 15) {
            jc_out_bits(o, hac->code[0xF0], hac->size[0xF0]); // ZRL
            run -= 16;
        }
        s = jc_category(v);
        int sym = (run << 4) | s;
        jc_out_bits(o, hac->code[sym], hac->size[sym]);
        int bits = v < 0 ? v + (1 << s) - 1 : v;
        jc_out_bits(o, (uint32_t)bits & ((1u << s) - 1), s);
        run = 0;
    }
    if (run > 0) {
        jc_out_bits(o, hac->code[0x00], hac->size[0x00]); // EOB
    }
}

static void jc_emit_dqt(JOut *o, int id, const uint8_t *qt_nat) {
    jc_out_be16(o, 0xFFDB);
    jc_out_be16(o, 67);
    jc_out_byte(o, (uint8_t)id);
    for (int k = 0; k < 64; k++) jc_out_byte(o, qt_nat[jc_zigzag[k]]);
}

static void jc_emit_dht(JOut *o, int class_id, const uint8_t bits[17],
                        const uint8_t *vals) {
    int total = 0;
    for (int l = 1; l <= 16; l++) total += bits[l];
    jc_out_be16(o, 0xFFC4);
    jc_out_be16(o, (uint16_t)(2 + 1 + 16 + total));
    jc_out_byte(o, (uint8_t)class_id);
    for (int l = 1; l <= 16; l++) jc_out_byte(o, bits[l]);
    jc_out_bytes(o, vals, (size_t)total);
}

// libjpeg quality scaling: q < 50 -> 5000/q, else 200 - 2q; each Annex K
// entry becomes (base*scale + 50)/100 clamped to 1..255.
static void jc_scale_qt(const uint8_t *base, int quality, uint8_t *out) {
    int scale = (quality < 50) ? 5000 / quality : 200 - 2 * quality;
    for (int k = 0; k < 64; k++) {
        int v = (base[k] * scale + 50) / 100;
        if (v < 1) v = 1;
        if (v > 255) v = 255;
        out[k] = (uint8_t)v;
    }
}

// Fill a padded plane by replicating the right column / bottom row out to the
// MCU-aligned dimensions (keeps block edges smooth, minimizing ringing).
static void jc_pad_plane(uint8_t *plane, int w, int h, int pw, int ph) {
    for (int y = 0; y < h; y++) {
        uint8_t v = plane[(size_t)y * pw + w - 1];
        memset(plane + (size_t)y * pw + w, v, (size_t)(pw - w));
    }
    for (int y = h; y < ph; y++) {
        memcpy(plane + (size_t)y * pw, plane + (size_t)(h - 1) * pw, (size_t)pw);
    }
}

bool tspdf_jpeg_encode(const TspdfRawImage *img, int quality,
                       TspdfArena *arena, uint8_t **out, size_t *out_len) {
    if (!img || !img->pixels || !arena || !out || !out_len) return false;
    if (quality < 1 || quality > 100) return false;
    if (img->width < 1 || img->height < 1) return false;
    if (img->width > JC_MAX_DIMENSION || img->height > JC_MAX_DIMENSION) return false;
    if ((int64_t)img->width * img->height > JC_MAX_PIXELS) return false;
    if (img->components != 1 && img->components != 3) return false;

    int W = img->width, H = img->height;
    bool color = (img->components == 3);

    uint8_t luma_qt[64], chroma_qt[64];
    jc_scale_qt(jc_std_luma_qt, quality, luma_qt);
    jc_scale_qt(jc_std_chroma_qt, quality, chroma_qt);
    float rq_luma[64], rq_chroma[64];
    for (int k = 0; k < 64; k++) {
        rq_luma[k] = 1.0f / (float)luma_qt[k];
        rq_chroma[k] = 1.0f / (float)chroma_qt[k];
    }

    float a[8][8];
    jc_dct_matrix(a);

    JEHuff dc_l, ac_l, dc_c, ac_c;
    jc_ehuff_build(&dc_l, jc_dc_luma_bits, jc_dc_luma_vals);
    jc_ehuff_build(&ac_l, jc_ac_luma_bits, jc_ac_luma_vals);
    jc_ehuff_build(&dc_c, jc_dc_chroma_bits, jc_dc_chroma_vals);
    jc_ehuff_build(&ac_c, jc_ac_chroma_bits, jc_ac_chroma_vals);

    // Build the (padded) sample planes. Color: Y at full resolution padded
    // to 16, Cb/Cr box-downsampled 2x2 (YCbCr is affine in RGB, so averaging
    // RGB first then converting equals converting then averaging, modulo
    // rounding) padded to 8. Gray: one plane padded to 8.
    int mcu = color ? 16 : 8;
    int pw = (W + mcu - 1) / mcu * mcu;
    int ph = (H + mcu - 1) / mcu * mcu;
    int cw = 0, ch = 0;
    uint8_t *py, *pcb = NULL, *pcr = NULL;
    py = (uint8_t *)tspdf_arena_alloc(arena, (size_t)pw * ph);
    if (!py) return false;

    const int32_t cy_r = JC_FIX(0.29900), cy_g = JC_FIX(0.58700), cy_b = JC_FIX(0.11400);
    const int32_t cb_r = JC_FIX(0.16874), cb_g = JC_FIX(0.33126), cb_b = JC_FIX(0.50000);
    const int32_t cr_r = JC_FIX(0.50000), cr_g = JC_FIX(0.41869), cr_b = JC_FIX(0.08131);

    if (!color) {
        for (int y = 0; y < H; y++) {
            memcpy(py + (size_t)y * pw, img->pixels + (size_t)y * W, (size_t)W);
        }
        jc_pad_plane(py, W, H, pw, ph);
    } else {
        cw = pw / 2;
        ch = ph / 2;
        pcb = (uint8_t *)tspdf_arena_alloc(arena, (size_t)cw * ch);
        pcr = (uint8_t *)tspdf_arena_alloc(arena, (size_t)cw * ch);
        if (!pcb || !pcr) return false;
        for (int y = 0; y < H; y++) {
            const uint8_t *row = img->pixels + (size_t)y * W * 3;
            uint8_t *yo = py + (size_t)y * pw;
            for (int x = 0; x < W; x++) {
                int r = row[x * 3], g = row[x * 3 + 1], b = row[x * 3 + 2];
                yo[x] = (uint8_t)((cy_r * r + cy_g * g + cy_b * b + 32768) >> 16);
            }
        }
        jc_pad_plane(py, W, H, pw, ph);
        int sw = (W + 1) / 2, sh = (H + 1) / 2; // significant chroma samples
        for (int y = 0; y < sh; y++) {
            uint8_t *cbo = pcb + (size_t)y * cw;
            uint8_t *cro = pcr + (size_t)y * cw;
            for (int x = 0; x < sw; x++) {
                // Average the up-to-2x2 RGB group (edge pixels count double
                // on odd dimensions), then convert.
                int x1 = (2 * x + 1 < W) ? 2 * x + 1 : W - 1;
                int y1 = (2 * y + 1 < H) ? 2 * y + 1 : H - 1;
                const uint8_t *p00 = img->pixels + ((size_t)(2 * y) * W + 2 * x) * 3;
                const uint8_t *p01 = img->pixels + ((size_t)(2 * y) * W + x1) * 3;
                const uint8_t *p10 = img->pixels + ((size_t)y1 * W + 2 * x) * 3;
                const uint8_t *p11 = img->pixels + ((size_t)y1 * W + x1) * 3;
                int r = (p00[0] + p01[0] + p10[0] + p11[0] + 2) >> 2;
                int g = (p00[1] + p01[1] + p10[1] + p11[1] + 2) >> 2;
                int b = (p00[2] + p01[2] + p10[2] + p11[2] + 2) >> 2;
                cbo[x] = (uint8_t)jc_clamp255(
                    128 + ((-cb_r * r - cb_g * g + cb_b * b + 32768) >> 16));
                cro[x] = (uint8_t)jc_clamp255(
                    128 + ((cr_r * r - cr_g * g - cr_b * b + 32768) >> 16));
            }
        }
        jc_pad_plane(pcb, sw, sh, cw, ch);
        jc_pad_plane(pcr, sw, sh, cw, ch);
    }

    JOut o;
    memset(&o, 0, sizeof(o));
    o.arena = arena;

    // --- Headers ---
    jc_out_be16(&o, 0xFFD8); // SOI
    static const uint8_t jfif[14] = {
        'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0
    };
    jc_out_be16(&o, 0xFFE0);
    jc_out_be16(&o, 16);
    jc_out_bytes(&o, jfif, sizeof(jfif));
    jc_emit_dqt(&o, 0, luma_qt);
    if (color) jc_emit_dqt(&o, 1, chroma_qt);
    jc_out_be16(&o, 0xFFC0); // SOF0
    jc_out_be16(&o, (uint16_t)(8 + 3 * (color ? 3 : 1)));
    jc_out_byte(&o, 8);
    jc_out_be16(&o, (uint16_t)H);
    jc_out_be16(&o, (uint16_t)W);
    jc_out_byte(&o, color ? 3 : 1);
    jc_out_byte(&o, 1); // Y
    jc_out_byte(&o, color ? 0x22 : 0x11);
    jc_out_byte(&o, 0);
    if (color) {
        jc_out_byte(&o, 2); jc_out_byte(&o, 0x11); jc_out_byte(&o, 1); // Cb
        jc_out_byte(&o, 3); jc_out_byte(&o, 0x11); jc_out_byte(&o, 1); // Cr
    }
    jc_emit_dht(&o, 0x00, jc_dc_luma_bits, jc_dc_luma_vals);
    jc_emit_dht(&o, 0x10, jc_ac_luma_bits, jc_ac_luma_vals);
    if (color) {
        jc_emit_dht(&o, 0x01, jc_dc_chroma_bits, jc_dc_chroma_vals);
        jc_emit_dht(&o, 0x11, jc_ac_chroma_bits, jc_ac_chroma_vals);
    }
    jc_out_be16(&o, 0xFFDA); // SOS
    jc_out_be16(&o, (uint16_t)(6 + 2 * (color ? 3 : 1)));
    jc_out_byte(&o, color ? 3 : 1);
    jc_out_byte(&o, 1); jc_out_byte(&o, 0x00);
    if (color) {
        jc_out_byte(&o, 2); jc_out_byte(&o, 0x11);
        jc_out_byte(&o, 3); jc_out_byte(&o, 0x11);
    }
    jc_out_byte(&o, 0);
    jc_out_byte(&o, 63);
    jc_out_byte(&o, 0);

    // --- Entropy-coded data ---
    int dc_y = 0, dc_cb = 0, dc_cr = 0;
    if (!color) {
        for (int by = 0; by < ph / 8; by++) {
            for (int bx = 0; bx < pw / 8; bx++) {
                jc_encode_block(&o, a, py + (size_t)by * 8 * pw + (size_t)bx * 8,
                                pw, rq_luma, &dc_l, &ac_l, &dc_y);
            }
        }
    } else {
        for (int my = 0; my < ph / 16; my++) {
            for (int mx = 0; mx < pw / 16; mx++) {
                for (int by = 0; by < 2; by++) {
                    for (int bx = 0; bx < 2; bx++) {
                        const uint8_t *src = py +
                            ((size_t)my * 16 + (size_t)by * 8) * pw +
                            (size_t)mx * 16 + (size_t)bx * 8;
                        jc_encode_block(&o, a, src, pw, rq_luma, &dc_l, &ac_l, &dc_y);
                    }
                }
                const uint8_t *cbs = pcb + (size_t)my * 8 * cw + (size_t)mx * 8;
                const uint8_t *crs = pcr + (size_t)my * 8 * cw + (size_t)mx * 8;
                jc_encode_block(&o, a, cbs, cw, rq_chroma, &dc_c, &ac_c, &dc_cb);
                jc_encode_block(&o, a, crs, cw, rq_chroma, &dc_c, &ac_c, &dc_cr);
            }
        }
    }
    jc_out_flush_bits(&o);
    jc_out_be16(&o, 0xFFD9); // EOI
    if (o.oom) return false;

    // Concatenate the chunk chain into one arena block.
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(arena, o.total);
    if (!buf) return false;
    size_t off = 0;
    for (JChunk *c = o.first; c; c = c->next) {
        memcpy(buf + off, c->data, c->len);
        off += c->len;
    }
    *out = buf;
    *out_len = o.total;
    return true;
}
