#include "deflate.h"
#include "../util/buffer.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// See TSPDF_DEFLATE_MAX_OUTPUT in deflate.h (documented public cap).

static bool deflate_out_has_room(const TspdfBuffer *out, size_t extra) {
    if (extra > TSPDF_DEFLATE_MAX_OUTPUT) return false;
    return out->len <= TSPDF_DEFLATE_MAX_OUTPUT - extra;
}

// ============================================================
// Deflate compressor with zlib wrapper (RFC 1950/1951)
//
// LZ77 with hash chains and lazy matching (zlib deflate_slow shape: every
// position is inserted into the chains, and emitting a match is deferred by
// one byte when the next position holds a longer one). Symbols are buffered
// per block; each block is emitted as whichever of stored (BTYPE=00), fixed
// Huffman (01), or dynamic Huffman (10) costs the fewest bits.
// ============================================================

// --- Bit writer ---

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint32_t bits;   // bit accumulator
    int nbits;       // number of valid bits in accumulator
    bool error;      // set true if any allocation failed
} BitWriter;

static void bw_init(BitWriter *bw, size_t initial_cap) {
    bw->buf = (uint8_t *)malloc(initial_cap);
    bw->cap = bw->buf ? initial_cap : 0;
    bw->len = 0;
    bw->bits = 0;
    bw->nbits = 0;
    bw->error = !bw->buf;
}

static void bw_ensure(BitWriter *bw, size_t extra) {
    // Guard len+extra against size_t wrap before comparing/growing: a wrapped
    // sum could read as < cap and skip the realloc, then bw_byte would write
    // past the buffer. bw_write_bytes passes stored-block payloads of up to
    // 65535 bytes here, so the guard is load-bearing, not just defensive.
    if (extra > SIZE_MAX - bw->len) { bw->error = true; return; }
    size_t needed = bw->len + extra;
    while (needed >= bw->cap) {
        size_t new_cap = bw->cap * 2;
        if (new_cap <= bw->cap) {
            // Doubling overflowed (or cap was 0): clamp to the exact need plus
            // headroom, guarding that "+1024 +1" addition against wrap too.
            if (needed > SIZE_MAX - 1025) { bw->error = true; return; }
            new_cap = needed + 1025;  // +1 for the '>=' loop condition, +1024 slack
        }
        uint8_t *p = (uint8_t *)realloc(bw->buf, new_cap);
        if (!p) { bw->error = true; return; }
        bw->buf = p;
        bw->cap = new_cap;
    }
}

static void bw_byte(BitWriter *bw, uint8_t b) {
    bw_ensure(bw, 1);
    if (bw->len >= bw->cap) return;  // alloc failed
    bw->buf[bw->len++] = b;
}

// Write n bits (LSB first, as per deflate spec)
static void bw_bits(BitWriter *bw, uint32_t value, int n) {
    bw->bits |= (value << bw->nbits);
    bw->nbits += n;
    while (bw->nbits >= 8) {
        bw_byte(bw, (uint8_t)(bw->bits & 0xFF));
        bw->bits >>= 8;
        bw->nbits -= 8;
    }
}

static void bw_flush(BitWriter *bw) {
    if (bw->nbits > 0) {
        bw_byte(bw, (uint8_t)(bw->bits & 0xFF));
        bw->bits = 0;
        bw->nbits = 0;
    }
}

// Append n raw bytes (used by stored blocks; caller must be byte-aligned).
static void bw_write_bytes(BitWriter *bw, const uint8_t *p, size_t n) {
    bw_ensure(bw, n);
    if (bw->error) return;
    memcpy(bw->buf + bw->len, p, n);
    bw->len += n;
}

// --- Length encoding (RFC 1951, section 3.2.5) ---

// Length: 3-258, encoded as lit/len symbols 257-285 + extra bits
static const int len_base[] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const int len_extra[] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

// Distance: 1-32768, encoded as dist symbols 0-29 + extra bits
static const int dist_base[] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289,
    16385,24577
};
static const int dist_extra[] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

// --- Encoder code tables ---
//
// Fixed Huffman codes (RFC 1951, section 3.2.6), precomputed bit-reversed for
// LSB-first emission, plus O(1) length->code and distance->code lookups.
//
// Note: these statics are initialized once on first call to deflate_compress.
// WARNING: Not thread-safe — concurrent first calls can race. The library is
// single-threaded by design. Call deflate_compress() once before spawning
// threads if you need to use this from multiple threads.

typedef struct { uint16_t code; uint8_t len; } HuffCode;

static HuffCode fixed_litlen[288];
static HuffCode fixed_dist[30];
static uint8_t length_code_tab[256];  // (match length - 3) -> len code 0..28
static uint8_t dist_code_tab[512];    // two-part lookup, see dist_code()
static bool enc_tables_ready = false;

static uint32_t reverse_bits(uint32_t code, int len) {
    uint32_t reversed = 0;
    for (int i = 0; i < len; i++) {
        reversed |= ((code >> i) & 1) << (len - 1 - i);
    }
    return reversed;
}

static void init_enc_tables(void) {
    if (enc_tables_ready) return;

    // Fixed lit/len:  0-143: 8 bits from 0x30;  144-255: 9 bits from 0x190;
    //                 256-279: 7 bits from 0;   280-287: 8 bits from 0xC0.
    for (int sym = 0; sym < 288; sym++) {
        uint32_t code;
        int len;
        if (sym <= 143) {
            code = 0x30 + sym; len = 8;
        } else if (sym <= 255) {
            code = 0x190 + (sym - 144); len = 9;
        } else if (sym <= 279) {
            code = 0x00 + (sym - 256); len = 7;
        } else {
            code = 0xC0 + (sym - 280); len = 8;
        }
        fixed_litlen[sym].code = (uint16_t)reverse_bits(code, len);
        fixed_litlen[sym].len = (uint8_t)len;
    }
    for (int sym = 0; sym < 30; sym++) {
        fixed_dist[sym].code = (uint16_t)reverse_bits(sym, 5);
        fixed_dist[sym].len = 5;
    }

    for (int c = 0; c < 28; c++) {
        for (int l = len_base[c]; l < len_base[c + 1] && l <= 257; l++) {
            length_code_tab[l - 3] = (uint8_t)c;
        }
    }
    length_code_tab[258 - 3] = 28;  // length 258 is its own code (285)

    // dist_code_tab: distances 1..256 index directly at (d-1); 257..32768 use
    // 256 + ((d-1)>>7), which works because all base ranges >= 257 are aligned
    // to 128-byte boundaries (same layout as zlib's _dist_code).
    for (int c = 0; c < 30; c++) {
        int hi = (c == 29) ? 32768 : dist_base[c + 1] - 1;
        for (int d = dist_base[c]; d <= hi; d++) {
            int idx = (d - 1) < 256 ? (d - 1) : 256 + ((d - 1) >> 7);
            dist_code_tab[idx] = (uint8_t)c;
        }
    }

    enc_tables_ready = true;
}

static int dist_code(int d) {
    return (d - 1) < 256 ? dist_code_tab[d - 1] : dist_code_tab[256 + ((d - 1) >> 7)];
}

// --- Length-limited canonical Huffman construction ---

#define MAX_HUFF_SYMS 286

typedef struct { uint32_t freq; uint16_t sym; } HuffLeaf;

static int huff_leaf_cmp(const void *pa, const void *pb) {
    const HuffLeaf *a = (const HuffLeaf *)pa;
    const HuffLeaf *b = (const HuffLeaf *)pb;
    if (a->freq != b->freq) return a->freq < b->freq ? -1 : 1;
    return a->sym < b->sym ? -1 : 1;  // deterministic tie-break
}

// Build code lengths (<= max_bits) for n symbols from their frequencies.
// Produces a complete code (Kraft sum exactly 2^max_bits) with at least two
// nonzero-length codes — mirroring zlib — so strict decoders that reject
// incomplete or over-subscribed tables accept it. lens[i] = 0 for unused syms.
static void huff_build_lengths(const uint32_t *freq, int n, int max_bits, uint8_t *lens) {
    HuffLeaf leaves[MAX_HUFF_SYMS];
    int m = 0;
    memset(lens, 0, (size_t)n);
    for (int i = 0; i < n; i++) {
        if (freq[i] > 0) leaves[m++] = (HuffLeaf){ freq[i], (uint16_t)i };
    }
    // Force at least two codes so degenerate blocks still yield a complete code.
    for (int i = 0; i < n && m < 2; i++) {
        bool present = false;
        for (int j = 0; j < m; j++) {
            if (leaves[j].sym == i) { present = true; break; }
        }
        if (!present) leaves[m++] = (HuffLeaf){ 1, (uint16_t)i };
    }
    qsort(leaves, (size_t)m, sizeof leaves[0], huff_leaf_cmp);

    // Standard Huffman over the sorted leaves via two-queue merge: internal
    // nodes are created in nondecreasing weight order, so no heap is needed.
    uint32_t w[2 * MAX_HUFF_SYMS];
    int par[2 * MAX_HUFF_SYMS];
    for (int i = 0; i < m; i++) w[i] = leaves[i].freq;
    int li = 0, ni = m;
    int total = 2 * m - 1;
    for (int next = m; next < total; next++) {
        int pick[2];
        for (int k = 0; k < 2; k++) {
            if (li < m && (ni >= next || w[li] <= w[ni])) pick[k] = li++;
            else pick[k] = ni++;
        }
        w[next] = w[pick[0]] + w[pick[1]];
        par[pick[0]] = next;
        par[pick[1]] = next;
    }
    int root = total - 1;

    // Leaf depths, clamped to max_bits; track the Kraft sum in units of
    // 2^-max_bits so the repair loops below can restore exact completeness.
    uint8_t depth[MAX_HUFF_SYMS];
    uint64_t kraft = 0;
    const uint64_t full = 1ull << max_bits;
    for (int i = 0; i < m; i++) {
        int d = 0;
        for (int j = i; j != root; j = par[j]) d++;
        if (d > max_bits) d = max_bits;
        depth[i] = (uint8_t)d;
        kraft += 1ull << (max_bits - d);
    }

    // Over-subscribed after clamping: lengthen the least-frequent leaf that is
    // deepest-but-still-below max_bits (smallest Kraft step, cheapest cost).
    while (kraft > full) {
        int pick = -1;
        for (int i = 0; i < m; i++) {
            if (depth[i] < max_bits && (pick < 0 || depth[i] > depth[pick])) pick = i;
        }
        kraft -= 1ull << (max_bits - depth[pick] - 1);
        depth[pick]++;
    }
    // Under-subscribed (the loop above can overshoot): shorten the most-
    // frequent deepest leaf whose Kraft step fits the remaining gap. A fitting
    // leaf always exists because both the gap and every step are multiples of
    // the smallest step present.
    while (kraft < full) {
        uint64_t gap = full - kraft;
        int pick = -1;
        for (int i = m - 1; i >= 0; i--) {
            if (depth[i] > 1 && (1ull << (max_bits - depth[i])) <= gap &&
                (pick < 0 || depth[i] > depth[pick])) pick = i;
        }
        kraft += 1ull << (max_bits - depth[pick]);
        depth[pick]--;
    }

    for (int i = 0; i < m; i++) lens[leaves[i].sym] = depth[i];
}

// Canonical code assignment (RFC 1951, section 3.2.2), bit-reversed for
// LSB-first emission.
static void lens_to_codes(const uint8_t *lens, int n, HuffCode *codes) {
    uint16_t bl_count[16] = {0};
    uint16_t next_code[16] = {0};
    for (int i = 0; i < n; i++) bl_count[lens[i]]++;
    bl_count[0] = 0;
    uint32_t code = 0;
    for (int bits = 1; bits <= 15; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        codes[i].len = lens[i];
        codes[i].code = lens[i] ? (uint16_t)reverse_bits(next_code[lens[i]]++, lens[i]) : 0;
    }
}

// --- Block symbol buffer ---

#define SYM_CAP (1u << 16)         // max symbols buffered per block
#define MAX_BLOCK_SRC (1u << 18)   // max input bytes covered per block

typedef struct { uint16_t len_or_lit; uint16_t dist; } LzSym;  // dist==0: literal

typedef struct {
    BitWriter *bw;
    const uint8_t *data;
    size_t len;
    LzSym *syms;
    uint32_t nsyms;
    uint32_t freq_ll[286];
    uint32_t freq_d[30];
    uint64_t extra_bits;    // total len/dist extra bits tallied in this block
    size_t block_start;     // input offset of the pending block's first byte
    size_t block_end;       // one past the last tallied input byte
    bool split_blocks;      // best level: split blocks adaptively at flush
} Enc;

static void tally_lit(Enc *e, uint8_t lit) {
    e->syms[e->nsyms].len_or_lit = lit;
    e->syms[e->nsyms].dist = 0;
    e->nsyms++;
    e->freq_ll[lit]++;
    e->block_end++;
}

static void tally_match(Enc *e, int length, int distance) {
    e->syms[e->nsyms].len_or_lit = (uint16_t)length;
    e->syms[e->nsyms].dist = (uint16_t)distance;
    e->nsyms++;
    int li = length_code_tab[length - 3];
    int di = dist_code(distance);
    e->freq_ll[257 + li]++;
    e->freq_d[di]++;
    e->extra_bits += (uint64_t)(len_extra[li] + dist_extra[di]);
    e->block_end += (size_t)length;
}

static bool block_full(const Enc *e) {
    return e->nsyms >= SYM_CAP || e->block_end - e->block_start >= MAX_BLOCK_SRC;
}

// --- Dynamic Huffman block header (RFC 1951, section 3.2.7) ---

static const uint8_t cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};
static const uint8_t cl_extra_bits[19] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7
};

typedef struct {
    uint8_t ll_lens[286];
    uint8_t d_lens[30];
    uint8_t cl_lens[19];
    HuffCode ll[286];
    HuffCode d[30];
    HuffCode cl[19];
    uint8_t item_sym[286 + 30];    // RLE'd code-length sequence
    uint8_t item_extra[286 + 30];  // extra-bits value per item
    int nitems;
    int hlit, hdist, hclen;
} DynPlan;

// RLE-encode the combined lit/len + dist code length sequence with symbols
// 0-15 (verbatim), 16 (repeat previous 3-6), 17 (3-10 zeros), 18 (11-138 zeros).
static int rle_lengths(const uint8_t *lens, int total, uint8_t *item_sym, uint8_t *item_extra) {
    int n = 0, i = 0;
    while (i < total) {
        uint8_t v = lens[i];
        int run = 1;
        while (i + run < total && lens[i + run] == v) run++;
        int r = run;
        if (v == 0) {
            while (r >= 11) {
                int t = r > 138 ? 138 : r;
                item_sym[n] = 18; item_extra[n] = (uint8_t)(t - 11); n++;
                r -= t;
            }
            if (r >= 3) {
                item_sym[n] = 17; item_extra[n] = (uint8_t)(r - 3); n++;
                r = 0;
            }
        } else {
            item_sym[n] = v; item_extra[n] = 0; n++;
            r--;
            while (r >= 3) {
                int t = r > 6 ? 6 : r;
                item_sym[n] = 16; item_extra[n] = (uint8_t)(t - 3); n++;
                r -= t;
            }
        }
        while (r-- > 0) { item_sym[n] = v; item_extra[n] = 0; n++; }
        i += run;
    }
    return n;
}

// Build the dynamic-Huffman plan for one block. Returns the bit cost of the
// header plus the Huffman-coded payload, excluding the 3-bit block preamble
// and the len/dist extra bits (both common to the fixed alternative).
static uint64_t build_dyn_plan(DynPlan *p, const uint32_t *freq_ll, const uint32_t *freq_d) {
    huff_build_lengths(freq_ll, 286, 15, p->ll_lens);
    huff_build_lengths(freq_d, 30, 15, p->d_lens);
    lens_to_codes(p->ll_lens, 286, p->ll);
    lens_to_codes(p->d_lens, 30, p->d);

    p->hlit = 257;
    for (int i = 285; i >= 257; i--) {
        if (p->ll_lens[i]) { p->hlit = i + 1; break; }
    }
    p->hdist = 1;
    for (int i = 29; i >= 1; i--) {
        if (p->d_lens[i]) { p->hdist = i + 1; break; }
    }

    uint8_t combined[286 + 30];
    memcpy(combined, p->ll_lens, (size_t)p->hlit);
    memcpy(combined + p->hlit, p->d_lens, (size_t)p->hdist);
    p->nitems = rle_lengths(combined, p->hlit + p->hdist, p->item_sym, p->item_extra);

    uint32_t cl_freq[19] = {0};
    for (int i = 0; i < p->nitems; i++) cl_freq[p->item_sym[i]]++;
    huff_build_lengths(cl_freq, 19, 7, p->cl_lens);
    lens_to_codes(p->cl_lens, 19, p->cl);

    p->hclen = 4;
    for (int i = 18; i >= 4; i--) {
        if (p->cl_lens[cl_order[i]]) { p->hclen = i + 1; break; }
    }

    uint64_t bits = 5 + 5 + 4 + (uint64_t)p->hclen * 3;
    for (int i = 0; i < p->nitems; i++) {
        bits += (uint64_t)p->cl_lens[p->item_sym[i]] + cl_extra_bits[p->item_sym[i]];
    }
    for (int s = 0; s < 286; s++) bits += (uint64_t)freq_ll[s] * p->ll_lens[s];
    for (int s = 0; s < 30; s++) bits += (uint64_t)freq_d[s] * p->d_lens[s];
    return bits;
}

// --- Block emission ---

static void emit_syms(BitWriter *bw, const LzSym *syms, uint32_t n,
                      const HuffCode *ll, const HuffCode *d) {
    for (uint32_t i = 0; i < n; i++) {
        if (syms[i].dist == 0) {
            const HuffCode *c = &ll[syms[i].len_or_lit];
            bw_bits(bw, c->code, c->len);
        } else {
            int length = syms[i].len_or_lit;
            int li = length_code_tab[length - 3];
            const HuffCode *c = &ll[257 + li];
            bw_bits(bw, c->code, c->len);
            if (len_extra[li]) bw_bits(bw, (uint32_t)(length - len_base[li]), len_extra[li]);
            int distance = syms[i].dist;
            int di = dist_code(distance);
            c = &d[di];
            bw_bits(bw, c->code, c->len);
            if (dist_extra[di]) bw_bits(bw, (uint32_t)(distance - dist_base[di]), dist_extra[di]);
        }
    }
    bw_bits(bw, ll[256].code, ll[256].len);  // end of block
}

// Emit the block's source bytes as stored (BTYPE=00) blocks, splitting at the
// 65535-byte stored-block limit. span must be > 0.
static void emit_stored(BitWriter *bw, const uint8_t *src, size_t span, bool final) {
    size_t off = 0;
    do {
        size_t chunk = span - off;
        if (chunk > 65535) chunk = 65535;
        bool last = final && off + chunk == span;
        bw_bits(bw, last ? 1u : 0u, 1);
        bw_bits(bw, 0, 2);
        bw_flush(bw);  // LEN/NLEN are byte-aligned
        uint16_t l = (uint16_t)chunk;
        bw_byte(bw, (uint8_t)(l & 0xFF));
        bw_byte(bw, (uint8_t)(l >> 8));
        bw_byte(bw, (uint8_t)(~l & 0xFF));
        bw_byte(bw, (uint8_t)((~l >> 8) & 0xFF));
        bw_write_bytes(bw, src + off, chunk);
        off += chunk;
    } while (off < span);
}

// Bit cost of the cheapest encoding for a block with the given statistics.
// freq_ll must already include the end-of-block symbol. *choice is 0 stored,
// 1 fixed, 2 dynamic; *plan is filled as a side effect of costing dynamic.
static uint64_t block_cost(const uint32_t *freq_ll, const uint32_t *freq_d,
                           uint64_t extra_bits, size_t span,
                           DynPlan *plan, int *choice) {
    uint64_t fixed_bits = 3 + extra_bits;
    for (int s = 0; s < 286; s++) fixed_bits += (uint64_t)freq_ll[s] * fixed_litlen[s].len;
    for (int s = 0; s < 30; s++) fixed_bits += (uint64_t)freq_d[s] * 5;

    uint64_t dyn_bits = 3 + build_dyn_plan(plan, freq_ll, freq_d) + extra_bits;

    uint64_t stored_bits = UINT64_MAX;
    if (span > 0) {
        uint64_t nchunks = ((uint64_t)span + 65534) / 65535;
        // Per chunk: 3 header bits + up to 5 alignment bits before LEN + 32
        // bits LEN/NLEN = 40 (every chunk realigns, not just the first).
        stored_bits = nchunks * 40 + (uint64_t)span * 8;
    }

    if (stored_bits <= fixed_bits && stored_bits <= dyn_bits) { *choice = 0; return stored_bits; }
    if (dyn_bits < fixed_bits) { *choice = 2; return dyn_bits; }
    *choice = 1;
    return fixed_bits;
}

// Emit syms[lo..hi) — covering span input bytes starting at input offset
// start — as one deflate block, using whichever encoding costs the least.
static void emit_block(Enc *e, uint32_t lo, uint32_t hi, size_t start, size_t span,
                       const uint32_t *freq_ll, const uint32_t *freq_d,
                       uint64_t extra_bits, bool final) {
    DynPlan plan;
    int choice;
    block_cost(freq_ll, freq_d, extra_bits, span, &plan, &choice);

    if (choice == 0) {
        emit_stored(e->bw, e->data + start, span, final);
    } else if (choice == 2) {
        bw_bits(e->bw, final ? 1u : 0u, 1);
        bw_bits(e->bw, 2, 2);
        bw_bits(e->bw, (uint32_t)(plan.hlit - 257), 5);
        bw_bits(e->bw, (uint32_t)(plan.hdist - 1), 5);
        bw_bits(e->bw, (uint32_t)(plan.hclen - 4), 4);
        for (int i = 0; i < plan.hclen; i++) {
            bw_bits(e->bw, plan.cl_lens[cl_order[i]], 3);
        }
        for (int i = 0; i < plan.nitems; i++) {
            int s = plan.item_sym[i];
            bw_bits(e->bw, plan.cl[s].code, plan.cl[s].len);
            if (cl_extra_bits[s]) bw_bits(e->bw, plan.item_extra[i], cl_extra_bits[s]);
        }
        emit_syms(e->bw, e->syms + lo, hi - lo, plan.ll, plan.d);
    } else {
        bw_bits(e->bw, final ? 1u : 0u, 1);
        bw_bits(e->bw, 1, 2);
        emit_syms(e->bw, e->syms + lo, hi - lo, fixed_litlen, fixed_dist);
    }
}

// Recompute block statistics for syms[lo..hi): symbol frequencies (including
// the end-of-block symbol), total extra bits, and covered input byte span.
static void tally_range(const Enc *e, uint32_t lo, uint32_t hi,
                        uint32_t *freq_ll, uint32_t *freq_d,
                        uint64_t *extra_bits, size_t *span) {
    memset(freq_ll, 0, 286 * sizeof *freq_ll);
    memset(freq_d, 0, 30 * sizeof *freq_d);
    *extra_bits = 0;
    *span = 0;
    for (uint32_t i = lo; i < hi; i++) {
        if (e->syms[i].dist == 0) {
            freq_ll[e->syms[i].len_or_lit]++;
            (*span)++;
        } else {
            int li = length_code_tab[e->syms[i].len_or_lit - 3];
            int di = dist_code(e->syms[i].dist);
            freq_ll[257 + li]++;
            freq_d[di]++;
            *extra_bits += (uint64_t)(len_extra[li] + dist_extra[di]);
            *span += e->syms[i].len_or_lit;
        }
    }
    freq_ll[256]++;
}

static uint64_t range_cost(const Enc *e, uint32_t lo, uint32_t hi, size_t *span) {
    uint32_t freq_ll[286], freq_d[30];
    uint64_t extra_bits;
    DynPlan plan;
    int choice;
    tally_range(e, lo, hi, freq_ll, freq_d, &extra_bits, span);
    return block_cost(freq_ll, freq_d, extra_bits, *span, &plan, &choice);
}

// Adaptive block splitting (best level): one Huffman table per 64K-symbol
// block leaves size on the table when symbol statistics shift mid-block
// (e.g. text next to binary). Recursively bisect the symbol range while two
// tailored blocks cost fewer estimated bits than one shared block; homogeneous
// data stays whole so tiny payloads don't pay for extra headers.
#define SPLIT_MIN_SYMS 1024u

static void emit_range(Enc *e, uint32_t lo, uint32_t hi, size_t start, bool final) {
    if (hi - lo >= 2 * SPLIT_MIN_SYMS) {
        uint32_t mid = lo + (hi - lo) / 2;
        size_t span_whole, span_left, span_right;
        uint64_t cost_whole = range_cost(e, lo, hi, &span_whole);
        uint64_t cost_left = range_cost(e, lo, mid, &span_left);
        uint64_t cost_right = range_cost(e, mid, hi, &span_right);
        if (cost_left + cost_right < cost_whole) {
            emit_range(e, lo, mid, start, false);
            emit_range(e, mid, hi, start + span_left, final);
            return;
        }
    }
    uint32_t freq_ll[286], freq_d[30];
    uint64_t extra_bits;
    size_t span;
    tally_range(e, lo, hi, freq_ll, freq_d, &extra_bits, &span);
    emit_block(e, lo, hi, start, span, freq_ll, freq_d, extra_bits, final);
}

// Close the pending block: pick the cheapest of stored/fixed/dynamic, emit it,
// and reset the block state. The best level may split the pending symbols
// into several blocks first (see emit_range).
static void flush_block(Enc *e, bool final) {
    if (e->split_blocks && e->nsyms > 0) {
        emit_range(e, 0, e->nsyms, e->block_start, final);
    } else {
        e->freq_ll[256]++;  // end-of-block symbol
        emit_block(e, 0, e->nsyms, e->block_start, e->block_end - e->block_start,
                   e->freq_ll, e->freq_d, e->extra_bits, final);
    }

    e->nsyms = 0;
    memset(e->freq_ll, 0, sizeof e->freq_ll);
    memset(e->freq_d, 0, sizeof e->freq_d);
    e->extra_bits = 0;
    e->block_start = e->block_end;
}

// --- LZ77 hash chain matcher ---

#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
#define WINDOW_SIZE 32768
#define MAX_MATCH 258
#define MIN_MATCH 3

// Search tuning. max_chain bounds chain walks per position; good_len quarters
// it when the deferred match is already good; nice_len stops a search early;
// max_lazy skips the deferred search entirely after a long match; too_far
// drops 3-byte matches whose distance costs more than three literals.
typedef struct {
    int max_chain;
    int good_len;
    int nice_len;
    int max_lazy;
    int too_far;
    int split_blocks;  // nonzero: adaptive Huffman-block splitting at flush
} MatchParams;

// Fast level: roughly zlib level 6-7 territory (see benchmark notes in the
// commit message). This is the default save-path compressor, so speed matters.
static const MatchParams match_fast = { 256, 8, 128, 64, 4096, 0 };
// Best level: zlib level 9 search parameters plus block splitting. ~4-10x
// slower, a few percent smaller; for callers who want maximum compression.
static const MatchParams match_best = { 4096, 32, 258, 258, 4096, 1 };

typedef struct {
    int32_t head[HASH_SIZE];   // hash -> most recent position, -1 if none
    int32_t prev[WINDOW_SIZE]; // ring: position -> previous position, same hash
} HashChain;

static uint32_t hash3(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 0x9E3779B1u) >> (32 - HASH_BITS);
}

static void hc_insert(HashChain *hc, const uint8_t *data, size_t pos) {
    uint32_t h = hash3(data + pos);
    hc->prev[pos & (WINDOW_SIZE - 1)] = hc->head[h];
    hc->head[h] = (int32_t)pos;
}

// Walk the hash chain from cand looking for a match at pos longer than
// floor_len. Returns the best length found (0 if none beat the floor) and
// sets *out_dist. Distances are capped at WINDOW_SIZE-1 so a prev[] ring slot
// can never be read after being overwritten by an aliasing newer position.
static int longest_match(const uint8_t *data, size_t len, size_t pos,
                         const int32_t *prev, int32_t cand, int floor_len,
                         int max_chain, int nice_len, int *out_dist) {
    const uint8_t *scan = data + pos;
    size_t avail = len - pos;
    int max_len = avail < MAX_MATCH ? (int)avail : MAX_MATCH;
    if (floor_len >= max_len) return 0;

    int best_len = floor_len;
    int best_dist = 0;
    int nice = nice_len < max_len ? nice_len : max_len;
    int chain = max_chain;

    while (cand >= 0 && chain-- > 0) {
        size_t dist = pos - (size_t)cand;
        if (dist >= WINDOW_SIZE) break;
        const uint8_t *m = data + cand;
        // Cheap rejects: last byte a longer match must extend, then first byte.
        if (m[best_len] == scan[best_len] && m[0] == scan[0]) {
            int l = 1;
            while (l + 8 <= max_len) {
                uint64_t x, y;
                memcpy(&x, scan + l, 8);
                memcpy(&y, m + l, 8);
                if (x != y) break;
                l += 8;
            }
            while (l < max_len && m[l] == scan[l]) l++;
            if (l > best_len) {
                best_len = l;
                best_dist = (int)dist;
                if (l >= nice) break;
                if (l >= max_len) break;
            }
        }
        cand = prev[cand & (WINDOW_SIZE - 1)];
    }

    if (best_dist == 0) return 0;
    *out_dist = best_dist;
    return best_len;
}

// --- Adler-32 checksum (RFC 1950) ---

static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    const size_t NMAX = 5552;
    size_t remaining = len;
    size_t pos = 0;

    while (remaining > 0) {
        size_t block = remaining > NMAX ? NMAX : remaining;
        for (size_t i = 0; i < block; i++) {
            a += data[pos + i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        pos += block;
        remaining -= block;
    }

    return (b << 16) | a;
}

// --- Main compress function ---

static uint8_t *deflate_compress_impl(const uint8_t *data, size_t len,
                                      size_t *out_len, const MatchParams *mp) {
    // Chain positions are int32; refuse absurd inputs rather than overflow.
    // Callers treat NULL as "store uncompressed", so this fails safe.
    if (len > (size_t)INT32_MAX - WINDOW_SIZE) return NULL;

    init_enc_tables();

    BitWriter bw;
    bw_init(&bw, len / 2 + 512);

    // Zlib header (RFC 1950): CMF = 0x78 (CM=8 deflate, CINFO=7 32K window),
    // FLG = FLEVEL=0 | FDICT=0 | FCHECK such that (CMF*256+FLG) % 31 == 0.
    bw_byte(&bw, 0x78);
    bw_byte(&bw, 0x01);

    Enc e = {0};
    e.bw = &bw;
    e.data = data;
    e.len = len;
    e.split_blocks = mp->split_blocks != 0;
    e.syms = (LzSym *)malloc(SYM_CAP * sizeof(LzSym));
    HashChain *hc = (HashChain *)malloc(sizeof(HashChain));
    if (!e.syms || !hc) {
        free(e.syms);
        free(hc);
        free(bw.buf);
        return NULL;
    }
    memset(hc->head, -1, sizeof(hc->head));
    // hc->prev needs no init: a slot is only ever read via a chain link that
    // was written when that position was inserted, and the distance cap in
    // longest_match rejects any link old enough for its slot to have aliased.

    // Lazy matching (zlib deflate_slow): a match found at pos-1 is held for
    // one byte; if pos yields a longer match, pos-1 is emitted as a literal.
    size_t pos = 0;
    int prev_len = 0, prev_dist = 0;
    bool match_avail = false;  // true when the byte at pos-1 is still pending

    while (pos < len) {
        int32_t cand = -1;
        if (pos + MIN_MATCH <= len) {
            uint32_t h = hash3(data + pos);
            cand = hc->head[h];
            hc->prev[pos & (WINDOW_SIZE - 1)] = cand;
            hc->head[h] = (int32_t)pos;
        }

        int cur_len = 0, cur_dist = 0;
        if (cand >= 0 && prev_len < mp->max_lazy) {
            int chain = mp->max_chain;
            if (prev_len >= mp->good_len) chain >>= 2;
            int floor_len = prev_len >= MIN_MATCH ? prev_len : MIN_MATCH - 1;
            cur_len = longest_match(data, len, pos, hc->prev, cand,
                                    floor_len, chain, mp->nice_len, &cur_dist);
            if (cur_len == MIN_MATCH && cur_dist > mp->too_far) cur_len = 0;
        }

        if (prev_len >= MIN_MATCH && cur_len <= prev_len) {
            // The deferred match starting at pos-1 wins.
            tally_match(&e, prev_len, prev_dist);
            // Insert the remaining positions covered by the match (pos is
            // already in; the loop top inserts the byte after the match).
            size_t match_end = pos - 1 + (size_t)prev_len;
            for (size_t k = pos + 1; k < match_end && k + MIN_MATCH <= len; k++) {
                hc_insert(hc, data, k);
            }
            pos = match_end;
            match_avail = false;
            prev_len = 0;
            prev_dist = 0;
            if (block_full(&e)) flush_block(&e, false);
        } else if (match_avail) {
            tally_lit(&e, data[pos - 1]);
            prev_len = cur_len;
            prev_dist = cur_dist;
            pos++;
            if (block_full(&e)) flush_block(&e, false);
        } else {
            match_avail = true;
            prev_len = cur_len;
            prev_dist = cur_dist;
            pos++;
        }
    }
    if (match_avail) tally_lit(&e, data[len - 1]);

    flush_block(&e, true);
    free(hc);
    free(e.syms);

    bw_flush(&bw);

    // Adler-32 checksum (big-endian)
    uint32_t checksum = adler32(data, len);
    bw_byte(&bw, (checksum >> 24) & 0xFF);
    bw_byte(&bw, (checksum >> 16) & 0xFF);
    bw_byte(&bw, (checksum >> 8) & 0xFF);
    bw_byte(&bw, checksum & 0xFF);

    if (bw.error) {
        free(bw.buf);
        return NULL;
    }

    *out_len = bw.len;
    return bw.buf;
}

uint8_t *deflate_compress(const uint8_t *data, size_t len, size_t *out_len) {
    return deflate_compress_impl(data, len, out_len, &match_fast);
}

uint8_t *deflate_compress_best(const uint8_t *data, size_t len, size_t *out_len) {
    return deflate_compress_impl(data, len, out_len, &match_best);
}

// ============================================================
// Inflate (decompressor) — RFC 1951 with zlib wrapper (RFC 1950)
// Supports block types 0 (stored), 1 (fixed Huffman), 2 (dynamic Huffman)
// ============================================================

// --- Bit reader ---

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;      // byte position
    uint32_t bits;   // bit buffer
    int nbits;       // valid bits
    bool overrun;    // set true once a read consumes more bits than are available
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->pos = 0;
    br->bits = 0;
    br->nbits = 0;
    br->overrun = false;
}

static void br_fill(BitReader *br) {
    while (br->nbits <= 24 && br->pos < br->len) {
        br->bits |= (uint32_t)br->data[br->pos++] << br->nbits;
        br->nbits += 8;
    }
}

static uint32_t br_read(BitReader *br, int n) {
    br_fill(br);
    // RFC 1951 streams carry no length field, so a truncated/corrupt stream can
    // ask for more bits than remain. Flag it instead of silently feeding zero
    // bits (which would yield garbage output that passes as success).
    if (br->nbits < n) br->overrun = true;
    uint32_t val = br->bits & ((1u << n) - 1);
    br->bits >>= n;
    br->nbits -= n;
    return val;
}

static void br_align(BitReader *br) {
    int discard = br->nbits & 7;
    br->bits >>= discard;
    br->nbits -= discard;
}

// --- Huffman tree for decoding ---

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMBOLS 320

typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1];   // number of codes of each length
    uint16_t symbols[HUFF_MAX_SYMBOLS];   // symbols sorted by code
} HuffTree;

static int huff_build(HuffTree *tree, const uint8_t *lengths, int count) {
    memset(tree->counts, 0, sizeof(tree->counts));
    for (int i = 0; i < count; i++) {
        if (lengths[i] > HUFF_MAX_BITS) return -1;
        tree->counts[lengths[i]]++;
    }
    tree->counts[0] = 0;

    // Compute offsets
    uint16_t offsets[HUFF_MAX_BITS + 1];
    offsets[0] = 0;
    offsets[1] = 0;
    for (int i = 1; i < HUFF_MAX_BITS; i++) {
        offsets[i + 1] = offsets[i] + tree->counts[i];
    }

    for (int i = 0; i < count; i++) {
        if (lengths[i] > 0) {
            tree->symbols[offsets[lengths[i]]++] = (uint16_t)i;
        }
    }
    return 0;
}

static int huff_decode(HuffTree *tree, BitReader *br) {
    br_fill(br);
    uint32_t code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        // Pulling a code bit past the end of a truncated stream would read a
        // bogus '0' bit; flag the overrun so the caller fails instead of
        // decoding garbage symbols.
        if (br->nbits <= 0) { br->overrun = true; return -1; }
        code |= (br->bits & 1);
        br->bits >>= 1;
        br->nbits--;

        int count = tree->counts[len];
        if ((int)(code - first) < count) {
            return tree->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;  // invalid
}

// Fixed Huffman trees (for block type 1)
static void build_fixed_trees(HuffTree *lit, HuffTree *dist) {
    uint8_t lengths[288];
    for (int i = 0; i <= 143; i++) lengths[i] = 8;
    for (int i = 144; i <= 255; i++) lengths[i] = 9;
    for (int i = 256; i <= 279; i++) lengths[i] = 7;
    for (int i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(lit, lengths, 288);

    uint8_t dlengths[32];
    for (int i = 0; i < 32; i++) dlengths[i] = 5;
    huff_build(dist, dlengths, 32);
}

// Decode dynamic Huffman trees (block type 2)
static int decode_dynamic_trees(BitReader *br, HuffTree *lit, HuffTree *dist) {
    int hlit = br_read(br, 5) + 257;
    int hdist = br_read(br, 5) + 1;
    int hclen = br_read(br, 4) + 4;

    static const int order[] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (int i = 0; i < hclen; i++) {
        cl_lengths[order[i]] = (uint8_t)br_read(br, 3);
    }

    HuffTree cl_tree;
    if (huff_build(&cl_tree, cl_lengths, 19) != 0) return -1;

    uint8_t lengths[288 + 32];
    int total = hlit + hdist;
    int i = 0;
    while (i < total) {
        int sym = huff_decode(&cl_tree, br);
        if (sym < 0) return -1;

        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return -1;
            int rep = br_read(br, 2) + 3;
            uint8_t prev = lengths[i - 1];
            for (int j = 0; j < rep && i < total; j++) lengths[i++] = prev;
        } else if (sym == 17) {
            int rep = br_read(br, 3) + 3;
            for (int j = 0; j < rep && i < total; j++) lengths[i++] = 0;
        } else if (sym == 18) {
            int rep = br_read(br, 7) + 11;
            for (int j = 0; j < rep && i < total; j++) lengths[i++] = 0;
        } else {
            return -1;
        }
    }

    if (huff_build(lit, lengths, hlit) != 0) return -1;
    if (huff_build(dist, lengths + hlit, hdist) != 0) return -1;
    return 0;
}

static int inflate_block(BitReader *br, HuffTree *lit, HuffTree *dist, TspdfBuffer *out) {
    for (;;) {
        int sym = huff_decode(lit, br);
        if (sym < 0) return -1;

        if (sym < 256) {
            if (!deflate_out_has_room(out, 1)) return -1;
            tspdf_buffer_append_byte(out, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;  // end of block
        } else {
            // Length/distance pair
            int li = sym - 257;
            if (li < 0 || li > 28) return -1;
            int length = len_base[li] + (len_extra[li] > 0 ? (int)br_read(br, len_extra[li]) : 0);

            int di = huff_decode(dist, br);
            if (di < 0 || di > 29) return -1;
            int distance = dist_base[di] + (dist_extra[di] > 0 ? (int)br_read(br, dist_extra[di]) : 0);

            // Copy from output buffer
            if ((size_t)distance > out->len) return -1;
            if (!deflate_out_has_room(out, (size_t)length)) return -1;
            for (int j = 0; j < length; j++) {
                tspdf_buffer_append_byte(out, out->data[out->len - distance]);
            }
        }
    }
}

uint8_t *deflate_decompress(const uint8_t *data, size_t len, size_t *out_len) {
    if (len < 6) return NULL;  // minimum: 2 header + 1 block + 4 checksum

    // Verify zlib header
    uint8_t cmf = data[0];
    // uint8_t flg = data[1];  // not strictly needed
    if ((cmf & 0x0F) != 8) return NULL;  // CM must be 8 (deflate)
    if (((cmf * 256 + data[1]) % 31) != 0) return NULL;  // FCHECK

    size_t infl_len = len - 6;

    size_t initial_cap;
    if (infl_len > SIZE_MAX / 4) {
        initial_cap = TSPDF_DEFLATE_MAX_OUTPUT;
    } else {
        initial_cap = infl_len * 4;
        if (initial_cap > TSPDF_DEFLATE_MAX_OUTPUT) {
            initial_cap = TSPDF_DEFLATE_MAX_OUTPUT;
        }
    }
    if (initial_cap < 16) initial_cap = 16;

    BitReader br;
    br_init(&br, data + 2, infl_len);  // skip header, leave 4 bytes for checksum

    TspdfBuffer out = tspdf_buffer_create(initial_cap);

    int bfinal;
    do {
        bfinal = (int)br_read(&br, 1);
        int btype = (int)br_read(&br, 2);

        // If reading the block header already ran past the end of the stream,
        // the data is truncated: bail rather than loop forever on a bfinal that
        // never turns 1 (br_read keeps returning 0 past EOF).
        if (br.overrun) {
            tspdf_buffer_destroy(&out);
            return NULL;
        }

        if (btype == 0) {
            // Stored (uncompressed) block
            br_align(&br);
            // br_fill pre-buffers whole bytes past the block header: rewind to
            // the true byte position before reading LEN/NLEN and the payload.
            br.pos -= (size_t)(br.nbits / 8);
            br.bits = 0;
            br.nbits = 0;
            if (br.pos + 4 > br.len) { tspdf_buffer_destroy(&out); return NULL; }
            // Read from underlying byte stream
            uint16_t block_len = br.data[br.pos] | ((uint16_t)br.data[br.pos + 1] << 8);
            uint16_t nlen = br.data[br.pos + 2] | ((uint16_t)br.data[br.pos + 3] << 8);
            if ((uint16_t)(block_len ^ nlen) != 0xFFFFu) {
                tspdf_buffer_destroy(&out);
                return NULL;
            }
            br.pos += 4;
            br.bits = 0;
            br.nbits = 0;
            if (br.pos + block_len > br.len) { tspdf_buffer_destroy(&out); return NULL; }
            if (!deflate_out_has_room(&out, block_len)) {
                tspdf_buffer_destroy(&out);
                return NULL;
            }
            for (int j = 0; j < block_len; j++) {
                tspdf_buffer_append_byte(&out, br.data[br.pos++]);
            }
        } else if (btype == 1) {
            // Fixed Huffman
            HuffTree lit_tree, dist_tree;
            build_fixed_trees(&lit_tree, &dist_tree);
            if (inflate_block(&br, &lit_tree, &dist_tree, &out) != 0) {
                tspdf_buffer_destroy(&out);
                return NULL;
            }
        } else if (btype == 2) {
            // Dynamic Huffman
            HuffTree lit_tree, dist_tree;
            if (decode_dynamic_trees(&br, &lit_tree, &dist_tree) != 0) {
                tspdf_buffer_destroy(&out);
                return NULL;
            }
            if (inflate_block(&br, &lit_tree, &dist_tree, &out) != 0) {
                tspdf_buffer_destroy(&out);
                return NULL;
            }
        } else {
            // Invalid block type
            tspdf_buffer_destroy(&out);
            return NULL;
        }
    } while (!bfinal);

    if (out.error) {
        tspdf_buffer_destroy(&out);
        return NULL;
    }

    // A truncated/corrupt deflate stream can finish the block loop after having
    // consumed bits it never had; reject it rather than return partial garbage.
    if (br.overrun) {
        tspdf_buffer_destroy(&out);
        return NULL;
    }

    // Verify Adler-32 checksum
    const uint8_t *chk = data + len - 4;
    uint32_t expected = ((uint32_t)chk[0] << 24) | ((uint32_t)chk[1] << 16) |
                        ((uint32_t)chk[2] << 8) | chk[3];
    uint32_t actual = adler32(out.data, out.len);
    if (expected != actual) {
        tspdf_buffer_destroy(&out);
        return NULL;
    }

    *out_len = out.len;
    return out.data;
}
