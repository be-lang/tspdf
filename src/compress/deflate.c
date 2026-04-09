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
// Minimal deflate compressor with zlib wrapper
// Uses fixed Huffman codes (block type 01) with LZ77 matching
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
    while (bw->len + extra >= bw->cap) {
        size_t new_cap = bw->cap * 2;
        if (new_cap <= bw->cap) { bw->cap = bw->len + extra + 1024; new_cap = bw->cap; }
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

// --- Fixed Huffman code tables (RFC 1951, section 3.2.6) ---
//
// Lit/Len 0-143:   8-bit codes  00110000 - 10111111  (0x30 - 0xBF reversed)
// Lit/Len 144-255: 9-bit codes  110010000 - 111111111
// Lit/Len 256-279: 7-bit codes  0000000 - 0010111
// Lit/Len 280-287: 8-bit codes  11000000 - 11000111

// Precomputed fixed Huffman codes (reversed for LSB-first emission)
typedef struct { uint16_t code; uint8_t len; } HuffCode;

// Note: these statics are initialized once on first call to deflate_compress.
// WARNING: Not thread-safe — concurrent first calls can race. The library is
// single-threaded by design. Call deflate_compress() once before spawning
// threads if you need to use this from multiple threads.
static HuffCode litlen_table[288];
static HuffCode dist_table[30];
static bool huffman_tables_ready = false;

static uint32_t reverse_bits(uint32_t code, int len) {
    uint32_t reversed = 0;
    for (int i = 0; i < len; i++) {
        reversed |= ((code >> i) & 1) << (len - 1 - i);
    }
    return reversed;
}

static void init_huffman_tables(void) {
    if (huffman_tables_ready) return;

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
        litlen_table[sym].code = (uint16_t)reverse_bits(code, len);
        litlen_table[sym].len = (uint8_t)len;
    }

    for (int sym = 0; sym < 30; sym++) {
        dist_table[sym].code = (uint16_t)reverse_bits(sym, 5);
        dist_table[sym].len = 5;
    }

    huffman_tables_ready = true;
}

static void emit_fixed_litlen(BitWriter *bw, int sym) {
    bw_bits(bw, litlen_table[sym].code, litlen_table[sym].len);
}

// Distance codes are 5 bits, fixed (0-29)
static void emit_fixed_dist(BitWriter *bw, int sym) {
    bw_bits(bw, dist_table[sym].code, dist_table[sym].len);
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

static int find_len_code(int length) {
    for (int i = 28; i >= 0; i--) {
        if (length >= len_base[i]) return i;
    }
    return 0;
}

static int find_dist_code(int distance) {
    for (int i = 29; i >= 0; i--) {
        if (distance >= dist_base[i]) return i;
    }
    return 0;
}

static void emit_match(BitWriter *bw, int length, int distance) {
    // Emit length
    int li = find_len_code(length);
    emit_fixed_litlen(bw, 257 + li);
    if (len_extra[li] > 0) {
        bw_bits(bw, length - len_base[li], len_extra[li]);
    }

    // Emit distance
    int di = find_dist_code(distance);
    emit_fixed_dist(bw, di);
    if (dist_extra[di] > 0) {
        bw_bits(bw, distance - dist_base[di], dist_extra[di]);
    }
}

// --- LZ77 hash chain matcher ---

#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)
#define WINDOW_SIZE 32768
#define MAX_MATCH 258
#define MIN_MATCH 3
#define MAX_CHAIN 64   // limit search depth for speed

static uint32_t hash3(const uint8_t *p) {
    return ((uint32_t)p[0] * 31 * 31 + (uint32_t)p[1] * 31 + (uint32_t)p[2]) & HASH_MASK;
}

typedef struct {
    int head[HASH_SIZE];   // hash -> most recent position
    int prev[WINDOW_SIZE]; // chain: position -> previous position with same hash
} HashChain;

static int find_match(const uint8_t *data, size_t len, size_t pos,
                      HashChain *hc, int *best_dist) {
    if (pos + MIN_MATCH > len) return 0;

    uint32_t h = hash3(data + pos);
    int best_len = MIN_MATCH - 1;
    *best_dist = 0;

    int chain_len = MAX_CHAIN;
    int p = hc->head[h];

    while (p >= 0 && chain_len-- > 0) {
        int dist = (int)pos - p;
        if (dist > WINDOW_SIZE || dist <= 0) break;

        // Check match length
        const uint8_t *a = data + pos;
        const uint8_t *b = data + p;
        int max_len = (int)(len - pos);
        if (max_len > MAX_MATCH) max_len = MAX_MATCH;

        int ml = 0;
        while (ml < max_len && a[ml] == b[ml]) ml++;

        if (ml > best_len) {
            best_len = ml;
            *best_dist = dist;
            if (ml == MAX_MATCH) break;
        }

        p = hc->prev[p & (WINDOW_SIZE - 1)];
    }

    return best_len >= MIN_MATCH ? best_len : 0;
}

static void insert_hash(HashChain *hc, const uint8_t *data, size_t pos) {
    uint32_t h = hash3(data + pos);
    hc->prev[pos & (WINDOW_SIZE - 1)] = hc->head[h];
    hc->head[h] = (int)pos;
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

uint8_t *deflate_compress(const uint8_t *data, size_t len, size_t *out_len) {
    BitWriter bw;
    bw_init(&bw, len + 256);  // start with generous allocation
    init_huffman_tables();

    // Zlib header (RFC 1950)
    // CMF: CM=8 (deflate), CINFO=7 (32K window)
    bw_byte(&bw, 0x78);
    // FLG: FCHECK so that (CMF*256 + FLG) % 31 == 0, FLEVEL=2 (default)
    // 0x78 * 256 = 30720; 30720 + FLG must be divisible by 31
    // 30720 / 31 = 990.967...; 31 * 991 = 30721; FLG = 1
    // But with FLEVEL=2: FLG = 0b_10_0_00001 = 0x01... wait let me just compute
    // FLEVEL=1 (fast), FDICT=0, FCHECK=?
    // 0x78 << 8 = 0x7800; need (0x7800 | FLG) % 31 == 0
    // 0x7800 % 31 = 30720 % 31 = 30720 - 990*31 = 30720 - 30690 = 30; FCHECK = 31-30=1
    // FLG = FLEVEL(bits7-6) | FDICT(bit5) | FCHECK(bits4-0)
    // FLEVEL=01 (fast) = 0x40; FLG = 0x40 | 1 = 0x41? check: (0x7841) % 31
    // 0x7841 = 30785; 30785/31 = 992.74... nope
    // Just use FLEVEL=0 (fastest): FLG = 0x00 | FCHECK
    // (0x7800 + FLG) % 31 == 0 → FLG & 0x1F = 1 → FLG = 0x01
    bw_byte(&bw, 0x01);

    // Emit single fixed Huffman block
    // BFINAL=1 (last block), BTYPE=01 (fixed Huffman)
    bw_bits(&bw, 1, 1);  // BFINAL
    bw_bits(&bw, 1, 2);  // BTYPE = 01

    if (len == 0) {
        // Just emit end-of-block
        emit_fixed_litlen(&bw, 256);
    } else {
        // Initialize hash chains
        HashChain *hc = (HashChain *)calloc(1, sizeof(HashChain));
        if (!hc) { free(bw.buf); return NULL; }
        memset(hc->head, -1, sizeof(hc->head));

        size_t pos = 0;
        while (pos < len) {
            int dist = 0;
            int match_len = 0;

            if (pos + MIN_MATCH <= len) {
                match_len = find_match(data, len, pos, hc, &dist);
            }

            if (match_len >= MIN_MATCH) {
                emit_match(&bw, match_len, dist);
                // Lazy insertion: only insert first position, skip the rest
                insert_hash(hc, data, pos);
                pos += match_len;
            } else {
                emit_fixed_litlen(&bw, data[pos]);
                if (pos + MIN_MATCH <= len) {
                    insert_hash(hc, data, pos);
                }
                pos++;
            }
        }

        // End of block
        emit_fixed_litlen(&bw, 256);
        free(hc);
    }

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
} BitReader;

static void br_init(BitReader *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->pos = 0;
    br->bits = 0;
    br->nbits = 0;
}

static void br_fill(BitReader *br) {
    while (br->nbits <= 24 && br->pos < br->len) {
        br->bits |= (uint32_t)br->data[br->pos++] << br->nbits;
        br->nbits += 8;
    }
}

static uint32_t br_read(BitReader *br, int n) {
    br_fill(br);
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

        if (btype == 0) {
            // Stored (uncompressed) block
            br_align(&br);
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
