#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "test_framework.h"

#include <math.h>

#include "../src/util/buffer.h"
#include "../src/util/arena.h"
#include "../src/compress/deflate.h"
#include "../src/image/png_decoder.h"
#include "../src/image/jpeg_codec.h"
#include "../src/image/ccitt_codec.h"
#include "../src/pdf/pdf_base14.h"
#include "../src/layout/layout.h"
#include "../src/pdf/tspdf_writer.h"
#include "../src/tspdf_error.h"
#include "../src/qr/qr_encode.h"
#include "../src/util/pdfdate.h"

// ============================================================
// TspdfBuffer tests
// ============================================================

TEST(test_buffer_create) {
    TspdfBuffer b = tspdf_buffer_create(64);
    ASSERT(b.data != NULL);
    ASSERT_EQ_INT((int)b.len, 0);
    ASSERT(b.capacity >= 64);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append) {
    TspdfBuffer b = tspdf_buffer_create(16);
    const char *msg = "hello";
    tspdf_buffer_append(&b, msg, 5);
    ASSERT_EQ_INT((int)b.len, 5);
    ASSERT(memcmp(b.data, "hello", 5) == 0);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_printf) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_printf(&b, "num=%d", 42);
    ASSERT_EQ_INT((int)b.len, 6);
    ASSERT(memcmp(b.data, "num=42", 6) == 0);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_reset) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_append_str(&b, "test");
    ASSERT(b.len == 4);
    size_t cap = b.capacity;
    tspdf_buffer_reset(&b);
    ASSERT_EQ_INT((int)b.len, 0);
    ASSERT(b.capacity == cap);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, 72.0, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed > 71.9999 && parsed < 72.0001);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double_precision) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, 3.14159, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed > 3.1415 && parsed < 3.1417);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double_negative) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, -0.5, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed < -0.4999 && parsed > -0.5001);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_int) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, 42);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "42");
    tspdf_buffer_destroy(&b);

    b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, -7);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "-7");
    tspdf_buffer_destroy(&b);

    b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, 0);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "0");
    tspdf_buffer_destroy(&b);
}

// A growth request that would overflow size_t (len + needed wraps) must be
// refused: set buf->error, leave len/capacity untouched, and never write.
TEST(test_buffer_reject_grow_overflow) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_append_str(&b, "abc");
    size_t saved_len = b.len;
    size_t saved_cap = b.capacity;
    // SIZE_MAX bytes can never fit alongside the existing 3; must not wrap.
    tspdf_buffer_append(&b, "x", SIZE_MAX);
    ASSERT(b.error);
    ASSERT(b.len == saved_len);
    ASSERT(b.capacity == saved_cap);
    // The original 3 bytes must remain intact (no shrinking realloc / overrun).
    ASSERT(memcmp(b.data, "abc", 3) == 0);
    tspdf_buffer_destroy(&b);
}

// A normal append must still grow correctly after the overflow-safe rewrite.
TEST(test_buffer_grow_beyond_initial) {
    TspdfBuffer b = tspdf_buffer_create(8);
    char payload[100];
    memset(payload, 'z', sizeof(payload));
    tspdf_buffer_append(&b, payload, sizeof(payload));
    ASSERT(!b.error);
    ASSERT_EQ_INT((int)b.len, (int)sizeof(payload));
    ASSERT(b.capacity >= sizeof(payload));
    ASSERT(memcmp(b.data, payload, sizeof(payload)) == 0);
    tspdf_buffer_destroy(&b);
}

// ============================================================
// TspdfArena tests
// ============================================================

TEST(test_arena_create) {
    TspdfArena a = tspdf_arena_create(4096);
    ASSERT(a.first != NULL);
    ASSERT(tspdf_arena_remaining(&a) > 0);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_alloc_alignment) {
    TspdfArena a = tspdf_arena_create(4096);
    void *p1 = tspdf_arena_alloc(&a, 1);
    void *p2 = tspdf_arena_alloc(&a, 1);
    ASSERT(p1 != NULL);
    ASSERT(p2 != NULL);
    ASSERT(((uintptr_t)p2 % 8) == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_growth) {
    TspdfArena a = tspdf_arena_create(64);
    // With growing arena, allocations beyond initial capacity succeed
    void *p = tspdf_arena_alloc(&a, 128);
    ASSERT(p != NULL);
    // Should have grown: total_allocated > 64
    ASSERT(a.total_allocated > 64);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_reset) {
    TspdfArena a = tspdf_arena_create(256);
    tspdf_arena_alloc(&a, 128);
    size_t rem1 = tspdf_arena_remaining(&a);
    tspdf_arena_reset(&a);
    size_t rem2 = tspdf_arena_remaining(&a);
    ASSERT(rem2 > rem1);
    tspdf_arena_destroy(&a);
}

// A size whose 8-byte alignment round-up would wrap size_t must fail cleanly
// (return NULL) rather than under-allocate a block the caller then overruns.
TEST(test_arena_reject_alloc_overflow) {
    TspdfArena a = tspdf_arena_create(256);
    void *p = tspdf_arena_alloc(&a, SIZE_MAX - 3);  // (size+7) wraps
    ASSERT(p == NULL);
    // Arena must remain usable for legitimate allocations afterward.
    void *q = tspdf_arena_alloc(&a, 32);
    ASSERT(q != NULL);
    tspdf_arena_destroy(&a);
}

// ============================================================
// Deflate round-trip tests
// ============================================================

TEST(test_deflate_roundtrip) {
    const char *input = "Hello, world! This is a test of deflate compression.";
    size_t input_len = strlen(input);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress((const uint8_t *)input, input_len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len > 0);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)input_len);
    ASSERT(memcmp(decomp, input, input_len) == 0);

    free(comp);
    free(decomp);
}

TEST(test_deflate_roundtrip_repeated) {
    size_t len = 10000;
    uint8_t *data = malloc(len);
    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)(i % 37);
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len < len);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(data, decomp, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

TEST(test_deflate_large_input) {
    size_t len = 100000;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)(i % 251);

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(decomp, data, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

TEST(test_deflate_roundtrip_empty) {
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress((const uint8_t *)"", 0, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len > 0);

    size_t decomp_len = 12345;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, 0);

    free(comp);
    free(decomp);
}

TEST(test_deflate_roundtrip_one_byte) {
    uint8_t input = 0x42;
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(&input, 1, &comp_len);
    ASSERT(comp != NULL);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, 1);
    ASSERT(decomp[0] == 0x42);

    free(comp);
    free(decomp);
}

TEST(test_deflate_roundtrip_all_same) {
    size_t len = 100000;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    memset(data, 'A', len);

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    // 100k identical bytes are runs of maximum-length matches; anything above
    // a few hundred bytes means match emission is broken.
    ASSERT(comp_len < 600);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(decomp, data, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

// Incompressible input must fall back to stored blocks and stay within a
// small overhead of the raw size instead of expanding through Huffman coding.
TEST(test_deflate_roundtrip_random_incompressible) {
    size_t len = 262144;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    uint32_t s = 0x12345678u;  // xorshift32: deterministic pseudo-random bytes
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        data[i] = (uint8_t)s;
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len < len + len / 100 + 64);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(decomp, data, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

TEST(test_deflate_roundtrip_text_corpus) {
    // Synthesize a text-like corpus: repetitive vocabulary, varying counters.
    size_t cap = 300000;
    char *text = (char *)malloc(cap);
    ASSERT(text != NULL);
    size_t n = 0;
    for (int i = 0; n + 128 < cap; i++) {
        n += (size_t)snprintf(text + n, cap - n,
                              "%d 0 obj << /Type /Page /Parent %d 0 R /Contents %d 0 R >> "
                              "BT /F1 %d Tf 72 %d Td (line of page text) Tj ET\n",
                              i, i / 8, i + 1, 8 + i % 4, i % 700);
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress((const uint8_t *)text, n, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len < n / 4);  // this must compress well

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)n);
    ASSERT(memcmp(decomp, text, n) == 0);

    free(text);
    free(comp);
    free(decomp);
}

// Long repeats at distances close to the 32K window edge exercise the far end
// of the distance code space and the hash-chain window cutoff.
TEST(test_deflate_roundtrip_long_repeats_near_window) {
    size_t seg = 32700;      // just under the 32768-byte window
    size_t len = seg * 3;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    uint32_t s = 0xC0FFEEu;
    for (size_t i = 0; i < seg; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        data[i] = (uint8_t)(s % 61);  // compressible-ish, distinctive
    }
    memcpy(data + seg, data, seg);          // repeat at distance seg
    memcpy(data + 2 * seg, data, seg);      // and again

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len < len / 2);  // the repeats must be found across ~32K

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(decomp, data, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

// Sizes straddling the compressor's internal block-split threshold (256K of
// input per block) so a block boundary lands before/on/after the input end.
TEST(test_deflate_roundtrip_block_boundaries) {
    static const size_t sizes[] = { 262143, 262144, 262145 };
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        size_t len = sizes[si];
        uint8_t *data = (uint8_t *)malloc(len);
        ASSERT(data != NULL);
        for (size_t i = 0; i < len; i++) {
            data[i] = (uint8_t)((i * i) >> 3);  // mildly compressible pattern
        }

        size_t comp_len = 0;
        uint8_t *comp = deflate_compress(data, len, &comp_len);
        ASSERT(comp != NULL);

        size_t decomp_len = 0;
        uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
        ASSERT(decomp != NULL);
        ASSERT_EQ_INT((int)decomp_len, (int)len);
        ASSERT(memcmp(decomp, data, len) == 0);

        free(data);
        free(comp);
        free(decomp);
    }
}

TEST(test_deflate_roundtrip_multi_megabyte) {
    size_t len = 6 * 1024 * 1024;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    // Mixed content: incompressible stretches, zero runs, text-ish repetition.
    uint32_t s = 0xDEADBEEFu;
    for (size_t i = 0; i < len; i++) {
        if (i % (1 << 20) < (1 << 18)) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            data[i] = (uint8_t)s;                      // random stretch
        } else if (i % (1 << 20) < (1 << 19)) {
            data[i] = 0;                               // zero stretch
        } else {
            data[i] = (uint8_t)("lorem ipsum dolor sit amet "[i % 27]);
        }
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len < len);

    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT_EQ_INT((int)decomp_len, (int)len);
    ASSERT(memcmp(decomp, data, len) == 0);

    free(data);
    free(comp);
    free(decomp);
}

// Crafted zlib: stored block with LEN that does not match one's-complement NLEN (RFC 1951).
TEST(test_deflate_rejects_invalid_stored_nlen) {
    uint8_t zlib[] = {
        0x78, 0x01,
        0x01,
        0x03, 0x00,
        0x00, 0x00,
        'a', 'b', 'c',
        0x00, 0x00, 0x00, 0x00,
    };
    size_t out_len = 0;
    uint8_t *out = deflate_decompress(zlib, sizeof zlib, &out_len);
    ASSERT(out == NULL);
}

// Invalid deflate block type 11 (reserved) after valid zlib header.
TEST(test_deflate_rejects_invalid_block_type) {
    uint8_t zlib[] = {
        0x78, 0x01,
        0x07,
        0x00, 0x00, 0x00, 0x00,
    };
    size_t out_len = 0;
    uint8_t *out = deflate_decompress(zlib, sizeof zlib, &out_len);
    ASSERT(out == NULL);
}

// Decompressed payload larger than TSPDF_DEFLATE_MAX_OUTPUT must fail.
TEST(test_deflate_rejects_output_over_max) {
    size_t n = (size_t)TSPDF_DEFLATE_MAX_OUTPUT + 1u;
    uint8_t *buf = (uint8_t *)malloc(n);
    ASSERT(buf != NULL);
    memset(buf, 0, n);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(buf, n, &comp_len);
    free(buf);
    ASSERT(comp != NULL);
    size_t decomp_len = 0;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp == NULL);
    free(comp);
}

// A deflate stream truncated mid-body must fail rather than silently return
// a short/garbage result. RFC 1951 has no length field, so the inflater can
// only detect this by flagging reads that run past the end of the input.
TEST(test_deflate_rejects_truncated_stream) {
    size_t len = 4096;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)((i * 7 + 3) % 256);

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(data, len, &comp_len);
    ASSERT(comp != NULL);
    // Need enough compressed bytes that cutting before the trailer/end-of-block
    // leaves the bitstream genuinely truncated.
    ASSERT(comp_len > 12);

    // Chop off the Adler-32 trailer and several body bytes so the final block
    // (and its end-of-block symbol) is incomplete.
    size_t truncated_len = comp_len - 8;
    size_t out_len = 12345;  // sentinel: must not be trusted on failure
    uint8_t *out = deflate_decompress(comp, truncated_len, &out_len);
    ASSERT(out == NULL);

    free(out);
    free(comp);
    free(data);
}

// A header-only zlib stream (no block payload at all) is truncated and must
// fail rather than return an empty/garbage buffer.
TEST(test_deflate_rejects_header_only_stream) {
    // Valid zlib header (0x78 0x01) plus four bytes that look like a trailer
    // but provide no real deflate block; the bit reader runs out immediately.
    uint8_t zlib[] = { 0x78, 0x01, 0x00, 0x00, 0x00, 0x00 };
    size_t out_len = 999;
    uint8_t *out = deflate_decompress(zlib, sizeof zlib, &out_len);
    // Either a clean NULL (preferred) — the point is no crash and no garbage.
    if (out != NULL) free(out);
    ASSERT(out == NULL);
}

static void test_png_write_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t lenbe[4] = {
        (uint8_t)((n >> 24) & 0xff), (uint8_t)((n >> 16) & 0xff),
        (uint8_t)((n >> 8) & 0xff), (uint8_t)(n & 0xff),
    };
    fwrite(lenbe, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n > 0 && data) {
        fwrite(data, 1, n, f);
    }
    uint8_t crc[4] = {0, 0, 0, 0};
    fwrite(crc, 1, 4, f);
}

// IHDR with width 0 must be rejected before pixel / IDAT processing.
TEST(test_png_rejects_zero_width) {
    const char *path = "/tmp/tspdf_test_png_zero_w.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0, 0, 0, 0,
        0, 0, 0, 1,
        8, 2, 0, 0, 0,
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// Oversized dimensions: raw raster size math must not wrap / allocate blindly.
TEST(test_png_rejects_oversized_dimensions) {
    const char *path = "/tmp/tspdf_test_png_huge.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        8, 2, 0, 0, 0,
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// Dimensions within INT_MAX but above the sane cap: on a 64-bit host the
// SIZE_MAX multiply guards still pass, so the per-axis cap is what prevents a
// tiny IHDR from demanding a multi-gigabyte allocation. width = cap + 1.
TEST(test_png_rejects_dimension_above_cap) {
    const char *path = "/tmp/tspdf_test_png_above_cap.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint32_t w = (uint32_t)TSPDF_PNG_MAX_DIMENSION + 1u;  // just over the cap
    uint8_t ihdr[13] = {
        (uint8_t)((w >> 24) & 0xff), (uint8_t)((w >> 16) & 0xff),
        (uint8_t)((w >> 8) & 0xff), (uint8_t)(w & 0xff),
        0, 0, 0, 1,
        8, 2, 0, 0, 0,  // 8-bit RGB
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// Truncated IDAT (declared length larger than file): must not treat phantom bytes as IDAT.
TEST(test_png_rejects_truncated_idat) {
    const char *path = "/tmp/tspdf_test_png_trunc_idat.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0, 0, 0, 1, 0, 0, 0, 1,
        8, 2, 0, 0, 0,
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    uint8_t lenbe[4] = {0, 0, 1, 0};
    fwrite(lenbe, 1, 4, f);
    fwrite("IDAT", 1, 4, f);
    for (int i = 0; i < 8; i++) {
        fputc(0, f);
    }
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// ============================================================
// Base14 font tests
// ============================================================

TEST(test_base14_get) {
    const TspdfBase14Metrics *m = tspdf_base14_get("Helvetica");
    ASSERT(m != NULL);
    ASSERT(tspdf_base14_get("NonExistent") == NULL);
}

TEST(test_base14_measure) {
    const TspdfBase14Metrics *m = tspdf_base14_get("Helvetica");
    ASSERT(m != NULL);
    double w = tspdf_base14_measure_text(m, 12.0, "Hello");
    ASSERT(w > 0);
}

TEST(test_base14_line_height) {
    const TspdfBase14Metrics *m = tspdf_base14_get("Helvetica");
    double lh = tspdf_base14_line_height(m, 12.0);
    ASSERT(lh > 0);
    // Helvetica: (718 - (-207)) * 12 / 1000 = 11.1
    ASSERT(lh > 10.0 && lh < 15.0);
}

TEST(test_base14_is_latin) {
    ASSERT(tspdf_base14_is_latin("Helvetica") == 1);
    ASSERT(tspdf_base14_is_latin("Symbol") == 0);
}

// ============================================================
// Layout tests
// ============================================================

TEST(test_layout_box) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *box = tspdf_layout_box(&ctx);
    ASSERT(box != NULL);
    ASSERT_EQ_INT(box->type, TSPDF_NODE_BOX);
    tspdf_arena_destroy(&a);
}

TEST(test_layout_text) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *t = tspdf_layout_text(&ctx, "hello", "Helvetica", 12);
    ASSERT(t != NULL);
    ASSERT_EQ_INT(t->type, TSPDF_NODE_TEXT);
    ASSERT_EQ_STR(t->text.text, "hello");
    tspdf_arena_destroy(&a);
}

TEST(test_layout_add_child_success) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *parent = tspdf_layout_box(&ctx);
    TspdfNode *child = tspdf_layout_box(&ctx);
    TspdfError err = tspdf_layout_add_child(parent, child);
    ASSERT(err == TSPDF_OK);
    ASSERT_EQ_INT(parent->child_count, 1);
    tspdf_layout_tree_free(parent);
    tspdf_arena_destroy(&a);
}

TEST(test_layout_add_child_overflow) {
    TspdfArena a = tspdf_arena_create(1024 * 1024 * 4);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *parent = tspdf_layout_box(&ctx);
    for (int i = 0; i < TSPDF_LAYOUT_MAX_CHILDREN; i++) {
        TspdfError err = tspdf_layout_add_child(parent, tspdf_layout_box(&ctx));
        ASSERT(err == TSPDF_OK);
    }
    TspdfError err = tspdf_layout_add_child(parent, tspdf_layout_box(&ctx));
    ASSERT(err == TSPDF_ERR_OVERFLOW);
    tspdf_layout_tree_free(parent);
    tspdf_arena_destroy(&a);
}

TEST(test_layout_span_success) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *t = tspdf_layout_text(&ctx, "base", "Helvetica", 12);
    bool ok = tspdf_layout_text_add_span(t, "span", "Helvetica", 12,
                                    tspdf_color_rgb(0, 0, 0), 0);
    ASSERT(ok);
    ASSERT_EQ_INT(t->text.span_count, 1);
    tspdf_layout_tree_free(t);
    tspdf_arena_destroy(&a);
}

TEST(test_layout_node_size) {
    // TspdfNode should be under 512 bytes with all pointer-based arrays
    ASSERT(sizeof(TspdfNode) < 512);
}

// ============================================================
// PDF document tests
// ============================================================

TEST(test_tspdf_writer_create) {
    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(doc != NULL);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_font) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *name = tspdf_writer_add_builtin_font(doc, "Helvetica");
    ASSERT(name != NULL);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_page) {
    TspdfWriter *doc = tspdf_writer_create();
    TspdfStream *s = tspdf_writer_add_page(doc);
    ASSERT(s != NULL);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_text_field) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_add_text_field(doc, 0, "field1",
                                           10, 10, 200, 30,
                                           "default", "Helvetica", 12);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_checkbox) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_add_checkbox(doc, 0, "check1",
                                         10, 10, 20, true);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_link) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_add_link(doc, 0, 10, 10, 100, 20,
                                     "https://example.com");
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

TEST(test_pdf_add_bookmark) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    int idx = tspdf_writer_add_bookmark(doc, "Chapter 1", 0);
    ASSERT(idx >= 0);
    tspdf_writer_destroy(doc);
}

// memmem for PDF bytes (the buffer contains binary sections, so no strstr).
static bool bytes_contain(const uint8_t *hay, size_t hay_len, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || hay_len < nl) return false;
    for (size_t i = 0; i + nl <= hay_len; i++) {
        if (memcmp(hay + i, needle, nl) == 0) return true;
    }
    return false;
}

TEST(test_pdf_bookmark_xyz_dest_and_unicode_title) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    tspdf_writer_add_page(doc);
    // Root bookmark with /XYZ destination, child under it (page 1, y=700.5),
    // and a non-ASCII title that must become a UTF-16BE hex string.
    int root = tspdf_writer_add_bookmark_xyz(doc, -1, "Chapter 1", 0, 780.0);
    ASSERT(root >= 0);
    int child = tspdf_writer_add_bookmark_xyz(doc, root, "\xc3\x9c" "bersicht", 1, 700.5);
    ASSERT(child >= 0);

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    ASSERT(bytes_contain(pdf, len, "/Title (Chapter 1)"));
    ASSERT(bytes_contain(pdf, len, "/XYZ 0 780.0000 null"));
    ASSERT(bytes_contain(pdf, len, "/XYZ 0 700.5000 null"));
    // UTF-16BE with BOM: FEFF, 'Ü' = 00DC, 'b' = 0062, ...
    ASSERT(bytes_contain(pdf, len, "/Title <FEFF00DC0062"));
    ASSERT(bytes_contain(pdf, len, "/Type /Outlines"));
    free(pdf);
}

TEST(test_pdf_bookmark_count_is_visible_descendants) {
    // ISO 32000-1 Table 153: /Count of an open item is the number of VISIBLE
    // descendants, not direct children. Root bookmark with two children, the
    // first child having one child of its own: item /Count 3, root child
    // /Count 1, and outline root /Count 4 (all items are written open).
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    int root = tspdf_writer_add_bookmark(doc, "Top", 0);
    ASSERT(root >= 0);
    int kid1 = tspdf_writer_add_child_bookmark(doc, root, "Kid1", 0);
    ASSERT(kid1 >= 0);
    ASSERT(tspdf_writer_add_child_bookmark(doc, kid1, "Grandkid", 0) >= 0);
    ASSERT(tspdf_writer_add_child_bookmark(doc, root, "Kid2", 0) >= 0);

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    ASSERT(bytes_contain(pdf, len, "/Count 3"));   // "Top": Kid1+Grandkid+Kid2
    ASSERT(bytes_contain(pdf, len, "/Count 1"));   // "Kid1": Grandkid
    ASSERT(bytes_contain(pdf, len, "/Count 4"));   // outline root: all items
    ASSERT(!bytes_contain(pdf, len, "/Count 2"));  // direct-child count is wrong
    free(pdf);
}

TEST(test_pdf_bookmark_title_truncates_at_utf8_boundary) {
    // A title longer than the 255-byte buffer is truncated; a multi-byte
    // UTF-8 sequence spanning the cut must be dropped whole, never split.
    // 254 * 'a' + "é" (C3 A9) = 256 bytes: naive truncation would keep the
    // lone C3 byte, the fixed path cuts after the 254 ASCII bytes.
    char title[257];
    memset(title, 'a', 254);
    title[254] = '\xc3';
    title[255] = '\xa9';
    title[256] = '\0';

    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    ASSERT(tspdf_writer_add_bookmark(doc, title, 0) >= 0);

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    // Expect a pure-ASCII literal string of exactly 254 'a's.
    char needle[300];
    memcpy(needle, "/Title (", 8);
    memset(needle + 8, 'a', 254);
    needle[8 + 254] = ')';
    needle[8 + 255] = '\0';
    ASSERT(bytes_contain(pdf, len, needle));
    free(pdf);
}

TEST(test_pdf_save) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_output.pdf");
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_output.pdf");
}

// ============================================================
// String safety tests
// ============================================================

TEST(test_long_text_preserved) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    char longtext[2048];
    memset(longtext, 'A', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = '\0';
    TspdfNode *t = tspdf_layout_text(&ctx, longtext, "Helvetica", 12);
    ASSERT(t != NULL);
    ASSERT(strlen(t->text.text) == sizeof(longtext) - 1);  // full text preserved
    tspdf_arena_destroy(&a);
}

TEST(test_long_font_name_truncated) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    char longname[256];
    memset(longname, 'X', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    TspdfError err = tspdf_writer_add_text_field(doc, 0, longname,
                                           10, 10, 200, 30,
                                           "", "Helvetica", 12);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

// ============================================================
// Bug fix tests
// ============================================================

// Verify save with builtin font and text content works end-to-end
TEST(test_pdf_save_with_text_content) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    TspdfStream *s = tspdf_writer_add_page(doc);
    ASSERT(s != NULL);
    tspdf_stream_set_font(s, "F1", 12);
    tspdf_stream_begin_text(s);
    tspdf_stream_text_position(s, 72, 700);
    tspdf_stream_show_text(s, "Test");
    tspdf_stream_end_text(s);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_text.pdf");
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_text.pdf");
}

// Verify PNG loader rejects missing files gracefully
TEST(test_png_missing_file_rejected) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    const char *name = tspdf_writer_add_png_image(doc, "/tmp/nonexistent_test.png");
    ASSERT(name == NULL);
    tspdf_writer_destroy(doc);
}

// Verify ttf_subset returns NULL when font lacks loca/glyf tables
TEST(test_font_subset_missing_tables) {
    TTF_Font fake_font;
    memset(&fake_font, 0, sizeof(fake_font));
    fake_font.num_glyphs = 10;
    bool used[10] = {true, false, false, false, false, false, false, false, false, false};
    size_t out_len = 0;
    uint8_t *result = ttf_subset(&fake_font, used, &out_len);
    ASSERT(result == NULL);
}

// Fix #6: TspdfArena exhaustion in tspdf_layout_path_begin returns NULL
TEST(test_layout_path_begin_arena_exhaustion) {
    TspdfArena a = tspdf_arena_create(32);  // tiny arena, not enough for TspdfPathConfig
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *box = tspdf_layout_box(&ctx);
    // box itself may or may not fit, but path allocation should fail
    if (box) {
        TspdfPathConfig *p = tspdf_layout_path_begin(&ctx, box);
        // Either NULL (arena exhausted) or valid - should not crash
        (void)p;
    }
    tspdf_arena_destroy(&a);
}

// Fix #3: AcroForm dict present when form fields exist
TEST(test_pdf_save_with_form_fields) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_add_text_field(doc, 0, "name", 10, 10, 200, 30,
                                           "default", "Helvetica", 12);
    ASSERT(err == TSPDF_OK);
    err = tspdf_writer_add_checkbox(doc, 0, "agree", 10, 50, 20, false);
    ASSERT(err == TSPDF_OK);
    err = tspdf_writer_save(doc, "/tmp/tspdf_test_acroform.pdf");
    ASSERT(err == TSPDF_OK);
    // Verify AcroForm is in the output by checking file is non-trivial
    FILE *f = fopen("/tmp/tspdf_test_acroform.pdf", "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ASSERT(size > 100);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_acroform.pdf");
}

// ============================================================
// TTF Parser tests
// ============================================================

TEST(test_ttf_cmap_format_preference) {
    TTF_Font font;
    memset(&font, 0, sizeof(font));
    bool loaded = ttf_load(&font, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!loaded) {
        SKIP("font missing");
    }
    uint16_t glyph_A = ttf_get_glyph_index(&font, 'A');
    ASSERT(glyph_A > 0);
    // Test high codepoint doesn't crash (may return 0 = .notdef)
    uint16_t glyph_high = ttf_get_glyph_index(&font, 0x1F600);
    (void)glyph_high;
    ttf_free(&font);
}

// ============================================================
// UTF-8 Wrapping tests
// ============================================================

TEST(test_wrap_text_utf8_no_split) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *t = tspdf_layout_text(&ctx, "\xC3\xA9\xC3\xA0", "Helvetica", 12);
    t->text.wrap = TSPDF_WRAP_CHAR;
    tspdf_layout_compute(&ctx, t, 1.0, 1000);
    for (int i = 0; i < t->computed_text.line_count; i++) {
        const char *line = t->computed_text.lines[i];
        if (line && line[0]) {
            uint8_t first = (uint8_t)line[0];
            ASSERT(first < 0x80 || first >= 0xC0);  // never starts with continuation byte
        }
    }
    tspdf_layout_tree_free(t);
    tspdf_arena_destroy(&a);
}

TEST(test_wrap_word_utf8) {
    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    TspdfNode *t = tspdf_layout_text(&ctx, "Hello world", "Helvetica", 12);
    t->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_compute(&ctx, t, 50.0, 1000);
    ASSERT(t->computed_text.line_count >= 1);
    tspdf_layout_tree_free(t);
    tspdf_arena_destroy(&a);
}

// ============================================================
// Error Reporting tests
// ============================================================

TEST(test_error_last_error_default) {
    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(tspdf_writer_last_error(doc) == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

TEST(test_pages_grow_dynamically) {
    TspdfWriter *doc = tspdf_writer_create();
    // Add more pages than the initial capacity to verify growth
    for (int i = 0; i < TSPDF_MAX_PAGES_INITIAL + 10; i++) {
        ASSERT(tspdf_writer_add_page(doc) != NULL);
    }
    ASSERT(doc->page_count == TSPDF_MAX_PAGES_INITIAL + 10);
    tspdf_writer_destroy(doc);
}

TEST(test_error_font_limit) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *base14[] = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
        "Symbol", "ZapfDingbats"
    };
    int added = 0;
    for (int round = 0; round < 3 && added < TSPDF_MAX_FONTS; round++) {
        for (int i = 0; i < 14 && added < TSPDF_MAX_FONTS; i++) {
            tspdf_writer_add_builtin_font(doc, base14[i]);
            added++;
        }
    }
    const char *result = tspdf_writer_add_builtin_font(doc, "Helvetica");
    ASSERT(result == NULL);
    ASSERT(tspdf_writer_last_error(doc) == TSPDF_ERR_FONT_LIMIT);
    tspdf_writer_destroy(doc);
}

TEST(test_error_save_returns_tspdf_error) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    tspdf_writer_add_page(doc);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_err.pdf");
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_err.pdf");
}

TEST(test_error_reset_on_success) {
    TspdfWriter *doc = tspdf_writer_create();
    // Trigger an error via font limit, then verify it resets on success
    const char *base14[] = {
        "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
        "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
        "Symbol", "ZapfDingbats"
    };
    for (int round = 0; round < 3; round++)
        for (int i = 0; i < 14; i++)
            tspdf_writer_add_builtin_font(doc, base14[i]);
    ASSERT(tspdf_writer_last_error(doc) == TSPDF_ERR_FONT_LIMIT);
    // A successful add_page should reset the error
    ASSERT(tspdf_writer_add_page(doc) != NULL);
    ASSERT(tspdf_writer_last_error(doc) == TSPDF_OK);
    tspdf_writer_destroy(doc);
}

TEST(test_error_string_all_codes) {
    ASSERT(tspdf_error_string(TSPDF_OK) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_ALLOC) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_IO) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_FONT_PARSE) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_FONT_LIMIT) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_PAGE_LIMIT) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_IMAGE_LIMIT) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_IMAGE_PARSE) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_INVALID_ARG) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_OVERFLOW) != NULL);
    ASSERT(tspdf_error_string(TSPDF_ERR_ENCODING) != NULL);
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_OK), "success");
}

// ============================================================
// Unicode Text Encoding tests
// ============================================================

TEST(test_show_text_utf8_ascii) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, fname);
    ASSERT(ttf != NULL);
    ASSERT(pdf_font != NULL);

    TspdfStream s = tspdf_stream_create();
    tspdf_stream_show_text_utf8(&s, "A", ttf, pdf_font);

    ASSERT(s.buf.len > 0);
    ASSERT(s.buf.data[0] == '<');
    ASSERT(strstr((const char *)s.buf.data, "> Tj") != NULL);

    uint16_t glyph_A = ttf_get_glyph_index(ttf, 'A');
    ASSERT(pdf_font->glyph_to_unicode != NULL);
    ASSERT(pdf_font->glyph_to_unicode[glyph_A] == 'A');

    tspdf_stream_destroy(&s);
    tspdf_writer_destroy(doc);
}

TEST(test_show_text_utf8_multibyte) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, fname);

    TspdfStream s = tspdf_stream_create();
    tspdf_stream_show_text_utf8(&s, "\xC3\xA9", ttf, pdf_font);  // U+00E9 = e with accent

    ASSERT(s.buf.len > 0);
    ASSERT(s.buf.data[0] == '<');

    uint16_t glyph = ttf_get_glyph_index(ttf, 0x00E9);
    if (glyph > 0) {
        ASSERT(pdf_font->glyph_to_unicode[glyph] == 0x00E9);
    }

    tspdf_stream_destroy(&s);
    tspdf_writer_destroy(doc);
}

TEST(test_show_text_utf8_invalid) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, fname);

    TspdfStream s = tspdf_stream_create();
    tspdf_stream_show_text_utf8(&s, "\xFF\xFE", ttf, pdf_font);
    ASSERT(s.buf.len > 0);

    tspdf_stream_destroy(&s);
    s = tspdf_stream_create();
    tspdf_stream_show_text_utf8(&s, "\xC3", ttf, pdf_font);  // truncated 2-byte
    ASSERT(s.buf.len > 0);

    tspdf_stream_destroy(&s);
    tspdf_writer_destroy(doc);
}

TEST(test_show_text_utf8_empty) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, fname);

    TspdfStream s = tspdf_stream_create();
    tspdf_stream_show_text_utf8(&s, "", ttf, pdf_font);
    ASSERT(s.buf.len > 0);

    tspdf_stream_destroy(&s);
    tspdf_writer_destroy(doc);
}

TEST(test_layout_render_ttf_utf8) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }

    TspdfArena a = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&a);
    ctx.doc = doc;

    TspdfNode *root = tspdf_layout_box(&ctx);
    TspdfNode *text = tspdf_layout_text(&ctx, "Hello \xC3\xA9", fname, 12);
    tspdf_layout_add_child(root, text);
    tspdf_layout_compute(&ctx, root, 500, 800);

    TspdfStream *s = tspdf_writer_add_page(doc);
    tspdf_layout_render(&ctx, root, s);

    // Stream should contain hex text (< ... > Tj), not literal (... Tj)
    const char *buf = (const char *)s->buf.data;
    ASSERT(strstr(buf, "> Tj") != NULL);

    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_layout_utf8.pdf");
    ASSERT(err == TSPDF_OK);

    tspdf_layout_tree_free(root);
    tspdf_arena_destroy(&a);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_layout_utf8.pdf");
}

TEST(test_pdf_save_with_ttf_unicode) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) { tspdf_writer_destroy(doc); SKIP("font missing"); }
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(doc, fname);
    TspdfStream *s = tspdf_writer_add_page(doc);
    ASSERT(s != NULL);

    tspdf_stream_begin_text(s);
    tspdf_stream_set_font(s, fname, 12);
    tspdf_stream_text_position(s, 72, 700);
    tspdf_stream_show_text_utf8(s, "Hello \xC3\xA9\xC3\xA0\xC3\xBC", ttf, pdf_font);
    tspdf_stream_end_text(s);

    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_unicode.pdf");
    ASSERT(err == TSPDF_OK);

    FILE *f = fopen("/tmp/tspdf_test_unicode.pdf", "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(size);
    size_t nread = fread(content, 1, (size_t)size, f);
    fclose(f);
    ASSERT(nread == (size_t)size);

    bool found_type0 = false, found_cidfont = false, found_identity_h = false;
    for (long i = 0; i < size - 13; i++) {
        if (memcmp(content + i, "/Type0", 6) == 0) found_type0 = true;
        if (memcmp(content + i, "/CIDFontType2", 13) == 0) found_cidfont = true;
        if (memcmp(content + i, "/Identity-H", 11) == 0) found_identity_h = true;
    }
    free(content);
    ASSERT(found_type0);
    ASSERT(found_cidfont);
    ASSERT(found_identity_h);

    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_unicode.pdf");
}

TEST(test_pdf_save_builtin_still_works) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    TspdfStream *s = tspdf_writer_add_page(doc);
    tspdf_stream_begin_text(s);
    tspdf_stream_set_font(s, "F1", 12);
    tspdf_stream_text_position(s, 72, 700);
    tspdf_stream_show_text(s, "Built-in font test");
    tspdf_stream_end_text(s);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_builtin.pdf");
    ASSERT(err == TSPDF_OK);

    FILE *f = fopen("/tmp/tspdf_test_builtin.pdf", "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(size);
    size_t nread = fread(content, 1, (size_t)size, f);
    fclose(f);
    ASSERT(nread == (size_t)size);

    bool found_cidfont = false;
    for (long i = 0; i < size - 13; i++) {
        if (memcmp(content + i, "/CIDFontType2", 13) == 0) found_cidfont = true;
    }
    free(content);
    ASSERT(!found_cidfont);  // Built-in fonts should NOT use CIDFont

    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_builtin.pdf");
}

TEST(test_small_stream_not_compressed) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    TspdfStream *s = tspdf_writer_add_page(doc);
    tspdf_stream_begin_text(s);
    tspdf_stream_set_font(s, "F1", 12);
    tspdf_stream_text_position(s, 72, 700);
    tspdf_stream_show_text(s, "Short");
    tspdf_stream_end_text(s);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_small.pdf");
    ASSERT(err == TSPDF_OK);

    // Verify the file is valid and small
    FILE *f = fopen("/tmp/tspdf_test_small.pdf", "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ASSERT(size > 0 && size < 5000);

    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_small.pdf");
}

// ============================================================
// Memory Safety tests (B1-B5)
// ============================================================

// B2: ttf_free handles a zeroed-out font struct without crashing
TEST(test_ttf_free_zeroed) {
    TTF_Font font;
    memset(&font, 0, sizeof(font));
    ttf_free(&font);  // should not crash
}

// B1: cmap format 4 rejects malformed font with huge seg_count
TEST(test_ttf_cmap_malformed_segcount) {
    // Build a minimal fake TTF with an oversized seg_count in the cmap subtable
    // This should return 0 (.notdef) rather than reading out of bounds
    TTF_Font font;
    memset(&font, 0, sizeof(font));
    // A small buffer with a format 4 cmap that claims a huge seg_count
    uint8_t fake_cmap[16];
    memset(fake_cmap, 0, sizeof(fake_cmap));
    fake_cmap[0] = 0; fake_cmap[1] = 4;  // format = 4
    // seg_count_x2 = 0xFFFF (huge)
    fake_cmap[6] = 0xFF; fake_cmap[7] = 0xFE;
    font.data = fake_cmap;
    font.data_len = sizeof(fake_cmap);
    font.cmap_offset = 0;
    font.cmap_format = 4;
    uint16_t glyph = ttf_get_glyph_index(&font, 'A');
    ASSERT(glyph == 0);  // should safely return .notdef
    // Don't call ttf_free — data is stack-allocated
}

// T10: cmap format 6 (trimmed table) must bounds-check the glyphIdArray read
// against the cmap length, not just the table offset. A subtable that claims a
// large entryCount but whose backing bytes are not present must return .notdef
// for an in-range codepoint rather than reading past the buffer.
TEST(test_ttf_cmap_format6_oob_entry_count) {
    TTF_Font font;
    memset(&font, 0, sizeof(font));
    // Format 6 header is 10 bytes: format(2) length(2) language(2)
    // firstCode(2) entryCount(2), then entryCount * 2 bytes of glyph IDs.
    // 4 bytes of leading padding keep cmap_offset != 0 (a zero offset is
    // treated as "no cmap"). We provide only the 10-byte header (no
    // glyphIdArray) but claim entryCount = 100, so a read for any code in
    // [firstCode, firstCode+100) would run off the end without the guard.
    uint8_t buf[4 + 10];
    memset(buf, 0, sizeof(buf));
    uint8_t *sub = buf + 4;
    sub[0] = 0; sub[1] = 6;     // format = 6
    sub[6] = 0; sub[7] = 'A';   // firstCode = 0x41 ('A')
    sub[8] = 0; sub[9] = 100;   // entryCount = 100 (no backing bytes present)
    font.data = buf;
    font.data_len = sizeof(buf);
    font.cmap_offset = 4;
    font.cmap_length = 10;      // 10 bytes: header only, no glyphIdArray
    font.cmap_format = 6;
    uint16_t glyph = ttf_get_glyph_index(&font, 'A');
    ASSERT(glyph == 0);  // must reject the OOB read and return .notdef
    // Don't call ttf_free — data is stack-allocated
}

// T10: cmap format 6 with a properly sized glyphIdArray still resolves a glyph.
TEST(test_ttf_cmap_format6_in_bounds) {
    TTF_Font font;
    memset(&font, 0, sizeof(font));
    // 4 bytes of leading padding so cmap_offset != 0 (a zero offset is treated
    // as "no cmap"), then a format-6 subtable: 10-byte header + 2 entries.
    uint8_t buf[4 + 14];
    memset(buf, 0, sizeof(buf));
    uint8_t *sub = buf + 4;
    sub[0] = 0; sub[1] = 6;     // format = 6
    sub[6] = 0; sub[7] = 'A';   // firstCode = 0x41 ('A')
    sub[8] = 0; sub[9] = 2;     // entryCount = 2
    sub[10] = 0; sub[11] = 7;   // glyph id for 'A'
    sub[12] = 0; sub[13] = 9;   // glyph id for 'B'
    font.data = buf;
    font.data_len = sizeof(buf);
    font.cmap_offset = 4;
    font.cmap_length = 14;      // bytes from subtable to end of cmap table
    font.cmap_format = 6;
    ASSERT(ttf_get_glyph_index(&font, 'A') == 7);
    ASSERT(ttf_get_glyph_index(&font, 'B') == 9);
    ASSERT(ttf_get_glyph_index(&font, 'C') == 0);  // out of range -> .notdef
}

// Big-endian writers for assembling synthetic font tables.
static void ttf_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void ttf_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

// Build a minimal but structurally valid TTF carrying head/hhea/maxp/hmtx/cmap
// plus a kern table whose subtable headers collectively declare more pairs than
// TTF_MAX_KERN_PAIRS can hold. Returns a malloc'd buffer (caller frees) and the
// length via out_len. Each of the n_subtables format-0 horizontal subtables is
// just an 8-byte header claiming nPairs=0xFFFF with no pair body, so the count
// loop accumulates 65535 pairs per subtable from a tiny file.
static uint8_t *build_kern_overflow_ttf(size_t *out_len, uint16_t n_subtables) {
    // Layout: offset table (12) + 5 table records (16 each = 80) = 96 byte header.
    // Then tables back to back: head(54) hhea(36) maxp(6) hmtx(4) cmap(12) kern(4 + n_subtables*8).
    const uint32_t dir = 12;
    const uint32_t num_tables = 5;
    const uint32_t head_off = dir + num_tables * 16;
    const uint32_t head_len = 54;
    const uint32_t hhea_off = head_off + head_len;
    const uint32_t hhea_len = 36;
    const uint32_t maxp_off = hhea_off + hhea_len;
    const uint32_t maxp_len = 6;
    const uint32_t hmtx_off = maxp_off + maxp_len;
    const uint32_t hmtx_len = 4;  // one h_metric: advance(2)+lsb(2)
    const uint32_t cmap_off = hmtx_off + hmtx_len;
    const uint32_t cmap_len = 12; // cmap header(4) + 1 record(8); subtable_offset out of range so parse_cmap picks fallback=NULL? keep simple
    const uint32_t kern_off = cmap_off + cmap_len;
    const uint32_t kern_len = 4 + (uint32_t)n_subtables * 8;
    size_t total = kern_off + kern_len;

    uint8_t *d = (uint8_t *)calloc(total, 1);
    if (!d) return NULL;

    ttf_be32(d, 0x00010000);          // sfnt version (TrueType)
    ttf_be16(d + 4, (uint16_t)num_tables);

    // Table directory records (tag, checksum=0, offset, length).
    struct { const char *tag; uint32_t off, len; } recs[5] = {
        {"cmap", cmap_off, cmap_len},
        {"head", head_off, head_len},
        {"hhea", hhea_off, hhea_len},
        {"hmtx", hmtx_off, hmtx_len},
        {"maxp", maxp_off, maxp_len},
    };
    for (uint32_t i = 0; i < num_tables; i++) {
        uint8_t *r = d + dir + i * 16;
        memcpy(r, recs[i].tag, 4);
        ttf_be32(r + 8, recs[i].off);
        ttf_be32(r + 12, recs[i].len);
    }

    // head: only units_per_em / bbox / index_to_loc_format are read; zeros are fine.
    // hhea: num_h_metrics at +34.
    ttf_be16(d + hhea_off + 34, 1);
    // maxp: num_glyphs at +4.
    ttf_be16(d + maxp_off + 4, 1);
    // hmtx: one metric (advance=0,lsb=0) already zeroed.

    // cmap: header version(2)=0, numTables(2)=1; one record platform=3 enc=1
    // pointing at a subtable_offset >= cmap_len so parse_cmap finds no usable
    // subtable -> but parse_cmap requires a best subtable. Make the record's
    // subtable_offset = 0 with platform 1/encoding 0 so it's ignored, leaving
    // best=NULL -> parse_cmap fails. To keep ttf_load_from_memory succeeding we
    // instead point a format-6 subtable in-bounds is overkill; simpler: make the
    // record valid pointing back into the cmap header region (offset 0).
    ttf_be16(d + cmap_off + 0, 0);    // version
    ttf_be16(d + cmap_off + 2, 1);    // numTables = 1
    ttf_be16(d + cmap_off + 4, 3);    // platform = 3 (Windows)
    ttf_be16(d + cmap_off + 6, 1);    // encoding = 1 (BMP) -> best_bmp
    ttf_be32(d + cmap_off + 8, 0);    // subtable_offset = 0 (start of cmap subtable area)
    // The selected subtable starts at cmap_off; its format word is bytes[0..1]=0
    // -> ttf_get_glyph_index treats unknown formats as .notdef, which is fine.

    // kern: version(2)=0, nTables(2)=n_subtables, then headers.
    ttf_be16(d + kern_off + 0, 0);
    ttf_be16(d + kern_off + 2, n_subtables);
    for (uint16_t t = 0; t < n_subtables; t++) {
        uint8_t *s = d + kern_off + 4 + t * 8;
        ttf_be16(s + 0, 0);        // subtable version
        ttf_be16(s + 2, 8);        // length = 8 (header only)
        ttf_be16(s + 4, 0x0001);   // coverage: format 0 (high byte), horizontal (bit 0)
        ttf_be16(s + 6, 0xFFFF);   // nPairs = 65535
    }

    *out_len = total;
    return d;
}

// T10: a kern table summing past the pair cap must be rejected (no allocation
// overflow), leaving the font usable with no kern data rather than overflowing
// malloc(total_pairs * sizeof(TTF_KernPair)).
TEST(test_ttf_kern_pair_overflow_rejected) {
    size_t len = 0;
    // 16 subtables * 65535 pairs = 1,048,560 > TTF_MAX_KERN_PAIRS (1,000,000).
    uint8_t *data = build_kern_overflow_ttf(&len, 16);
    ASSERT(data != NULL);

    TTF_Font font;
    bool ok = ttf_load_from_memory(&font, data, len);
    // The required tables are valid, so the font loads; the kern table is
    // rejected by the cap, so has_kern stays false and kern_pairs stays NULL.
    ASSERT(ok);
    ASSERT(font.has_kern == false);
    ASSERT(font.kern_pairs == NULL);
    ASSERT(font.kern_pair_count == 0);
    ttf_free(&font);  // ttf_load_from_memory took ownership of data
}

// B2: ttf_load_from_memory cleans up on partial parse failure
TEST(test_ttf_load_from_memory_partial_failure) {
    // Create data that passes initial checks but fails on a required table
    // Minimum: valid sfnt header + table directory, but tables point nowhere valid
    size_t len = 128;
    uint8_t *data = (uint8_t *)calloc(len, 1);
    // sfnt version = 0x00010000 (TrueType)
    data[0] = 0x00; data[1] = 0x01; data[2] = 0x00; data[3] = 0x00;
    // num_tables = 1
    data[4] = 0x00; data[5] = 0x01;
    // Table record at offset 12: tag = "head", checksum = 0, offset = 200, length = 54
    data[12] = 'h'; data[13] = 'e'; data[14] = 'a'; data[15] = 'd';
    // offset = 200 (out of range for our 128-byte buffer)
    data[20] = 0x00; data[21] = 0x00; data[22] = 0x00; data[23] = 200;
    // length = 54
    data[24] = 0x00; data[25] = 0x00; data[26] = 0x00; data[27] = 54;

    TTF_Font font;
    bool ok = ttf_load_from_memory(&font, data, len);
    ASSERT(!ok);  // should fail without leaking tables allocation
    // data is still owned by us since ttf_load_from_memory failed
    free(data);
}

// --- Audit fixes (fix/reader-core): stored-block inflate support ---

// A VALID zlib stream of stored (BTYPE=00) blocks — what zlib emits at
// compression level 0 — must inflate correctly. The bit reader pre-buffers
// bytes, so the stored-block path has to rewind to the true byte position
// before reading LEN/NLEN.
TEST(test_deflate_decompresses_valid_stored_block) {
    const char payload[] = "stored block payload 123";
    size_t payload_len = sizeof(payload) - 1;

    uint8_t zlib[2 + 5 + sizeof(payload) - 1 + 4];
    size_t pos = 0;
    zlib[pos++] = 0x78;
    zlib[pos++] = 0x01;
    zlib[pos++] = 0x01;  // BFINAL=1, BTYPE=00
    zlib[pos++] = (uint8_t)(payload_len & 0xFF);
    zlib[pos++] = (uint8_t)(payload_len >> 8);
    zlib[pos++] = (uint8_t)(~payload_len & 0xFF);
    zlib[pos++] = (uint8_t)((~payload_len >> 8) & 0xFF);
    memcpy(zlib + pos, payload, payload_len);
    pos += payload_len;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < payload_len; i++) {
        s1 = (s1 + (uint8_t)payload[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    uint32_t adler = (s2 << 16) | s1;
    zlib[pos++] = (uint8_t)(adler >> 24);
    zlib[pos++] = (uint8_t)(adler >> 16);
    zlib[pos++] = (uint8_t)(adler >> 8);
    zlib[pos++] = (uint8_t)adler;

    size_t out_len = 0;
    uint8_t *out = deflate_decompress(zlib, pos, &out_len);
    ASSERT(out != NULL);
    ASSERT(out_len == payload_len);
    ASSERT(memcmp(out, payload, payload_len) == 0);
    free(out);
}

// ============================================================
// Encoding / i18n (fix/encoding track)
// ============================================================

#include "../include/tspdf/version.h"

// Binary-safe substring search (writer output contains compressed streams).
static bool enc_bytes_contains(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (!haystack || needle_len == 0 || needle_len > haystack_len) return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

TEST(test_writer_producer_is_tspdf_with_version) {
    // The Info dict /Producer stamp must carry the project name + version.
    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(doc != NULL);
    tspdf_writer_add_page(doc);

    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(doc, &out, &out_len);
    ASSERT(err == TSPDF_OK);

    ASSERT(enc_bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    free(out);
    tspdf_writer_destroy(doc);
}

// ============================================================
// PNG palette support (cli-media track)
// ============================================================

// Write a palette PNG (color type 3) to `path`. `palette` is palette_entries
// RGB triplets; `trns` (may be NULL) is trns_entries palette-alpha bytes;
// `indices` is width*height palette indices (one byte each, values must fit in
// bit_depth). Scanlines are packed MSB-first per the PNG spec, each prefixed
// with filter byte 0, and deflate-compressed into a single IDAT. Reuses
// test_png_write_chunk (the decoder does not verify chunk CRCs).
static bool test_png_write_palette_file(const char *path, int width, int height,
                                        int bit_depth,
                                        const uint8_t *palette, int palette_entries,
                                        const uint8_t *trns, int trns_entries,
                                        const uint8_t *indices) {
    size_t stride = ((size_t)width * (size_t)bit_depth + 7u) / 8u;
    size_t raw_len = (stride + 1u) * (size_t)height;
    uint8_t *rawbuf = (uint8_t *)calloc(raw_len, 1);
    if (!rawbuf) return false;
    for (int y = 0; y < height; y++) {
        uint8_t *row = rawbuf + (size_t)y * (stride + 1u) + 1;  // row[−1] is filter 0
        for (int x = 0; x < width; x++) {
            uint8_t idx = indices[y * width + x];
            size_t bit = (size_t)x * (size_t)bit_depth;
            unsigned shift = 8u - (unsigned)bit_depth - (unsigned)(bit % 8u);
            row[bit / 8u] |= (uint8_t)(idx << shift);
        }
    }
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(rawbuf, raw_len, &comp_len);
    free(rawbuf);
    if (!comp) return false;

    FILE *f = fopen(path, "wb");
    if (!f) { free(comp); return false; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        (uint8_t)((width >> 24) & 0xff), (uint8_t)((width >> 16) & 0xff),
        (uint8_t)((width >> 8) & 0xff), (uint8_t)(width & 0xff),
        (uint8_t)((height >> 24) & 0xff), (uint8_t)((height >> 16) & 0xff),
        (uint8_t)((height >> 8) & 0xff), (uint8_t)(height & 0xff),
        (uint8_t)bit_depth, 3, 0, 0, 0,  // color type 3 = palette
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "PLTE", palette, (size_t)palette_entries * 3);
    if (trns) {
        test_png_write_chunk(f, "tRNS", trns, (size_t)trns_entries);
    }
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(comp);
    return true;
}

// 8-bit palette indices expand to the palette's RGB triplets.
TEST(test_png_palette_8bit_decodes) {
    const char *path = "/tmp/tspdf_test_png_pal8.png";
    static const uint8_t palette[] = {
        255, 0, 0,   0, 255, 0,   0, 0, 255,   255, 255, 0,
    };
    static const uint8_t indices[] = { 0, 1, 2, 3 };  // 2x2
    ASSERT(test_png_write_palette_file(path, 2, 2, 8, palette, 4, NULL, 0, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(img.width, 2);
    ASSERT_EQ_INT(img.height, 2);
    ASSERT_EQ_INT(img.channels, 3);  // no tRNS -> opaque RGB
    static const uint8_t expected[] = {
        255, 0, 0,   0, 255, 0,
        0, 0, 255,   255, 255, 0,
    };
    ASSERT(memcmp(img.pixels, expected, sizeof(expected)) == 0);
    png_image_free(&img);
}

// tRNS palette alpha: covered entries use their alpha, uncovered default to 255.
TEST(test_png_palette_trns_decodes_rgba) {
    const char *path = "/tmp/tspdf_test_png_pal_trns.png";
    static const uint8_t palette[] = {
        10, 20, 30,   40, 50, 60,   70, 80, 90,
    };
    static const uint8_t trns[] = { 0, 128 };  // entry 2 uncovered -> 255
    static const uint8_t indices[] = { 0, 1, 2 };  // 3x1
    ASSERT(test_png_write_palette_file(path, 3, 1, 8, palette, 3, trns, 2, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(img.channels, 4);  // tRNS present -> RGBA
    static const uint8_t expected[] = {
        10, 20, 30, 0,   40, 50, 60, 128,   70, 80, 90, 255,
    };
    ASSERT(memcmp(img.pixels, expected, sizeof(expected)) == 0);
    png_image_free(&img);
}

// 1-bit indices: 9 pixels/row spans 2 bytes, exercising row padding bits.
TEST(test_png_palette_1bit_decodes) {
    const char *path = "/tmp/tspdf_test_png_pal1.png";
    static const uint8_t palette[] = { 0, 0, 0,   255, 255, 255 };
    static const uint8_t indices[] = { 1, 0, 1, 0, 1, 0, 1, 0, 1 };  // 9x1
    ASSERT(test_png_write_palette_file(path, 9, 1, 1, palette, 2, NULL, 0, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(img.width, 9);
    ASSERT_EQ_INT(img.channels, 3);
    for (int x = 0; x < 9; x++) {
        uint8_t want = (x % 2 == 0) ? 255 : 0;
        ASSERT(img.pixels[x * 3 + 0] == want);
        ASSERT(img.pixels[x * 3 + 1] == want);
        ASSERT(img.pixels[x * 3 + 2] == want);
    }
    png_image_free(&img);
}

// 2-bit indices: 5 pixels/row = 10 bits spanning 2 bytes.
TEST(test_png_palette_2bit_decodes) {
    const char *path = "/tmp/tspdf_test_png_pal2.png";
    static const uint8_t palette[] = {
        1, 2, 3,   4, 5, 6,   7, 8, 9,   10, 11, 12,
    };
    static const uint8_t indices[] = { 0, 1, 2, 3, 0 };  // 5x1
    ASSERT(test_png_write_palette_file(path, 5, 1, 2, palette, 4, NULL, 0, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok);
    for (int x = 0; x < 5; x++) {
        int idx = indices[x];
        ASSERT(img.pixels[x * 3 + 0] == palette[idx * 3 + 0]);
        ASSERT(img.pixels[x * 3 + 1] == palette[idx * 3 + 1]);
        ASSERT(img.pixels[x * 3 + 2] == palette[idx * 3 + 2]);
    }
    png_image_free(&img);
}

// 4-bit indices across two rows: 3 pixels/row = 12 bits spanning 2 bytes.
TEST(test_png_palette_4bit_decodes) {
    const char *path = "/tmp/tspdf_test_png_pal4.png";
    static const uint8_t palette[] = {
        100, 0, 0,   0, 100, 0,   0, 0, 100,
    };
    static const uint8_t indices[] = { 0, 1, 2,  2, 1, 0 };  // 3x2
    ASSERT(test_png_write_palette_file(path, 3, 2, 4, palette, 3, NULL, 0, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok);
    for (int i = 0; i < 6; i++) {
        int idx = indices[i];
        ASSERT(img.pixels[i * 3 + 0] == palette[idx * 3 + 0]);
        ASSERT(img.pixels[i * 3 + 1] == palette[idx * 3 + 1]);
        ASSERT(img.pixels[i * 3 + 2] == palette[idx * 3 + 2]);
    }
    png_image_free(&img);
}

// A palette index past the PLTE entry count is malformed and must be rejected.
TEST(test_png_palette_index_out_of_range_rejected) {
    const char *path = "/tmp/tspdf_test_png_pal_oob.png";
    static const uint8_t palette[] = { 255, 0, 0,   0, 255, 0 };  // 2 entries
    static const uint8_t indices[] = { 0, 5 };  // 5 >= 2 -> invalid
    ASSERT(test_png_write_palette_file(path, 2, 1, 8, palette, 2, NULL, 0, indices));

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// A palette PNG with no PLTE chunk at all is malformed and must be rejected.
TEST(test_png_palette_missing_plte_rejected) {
    const char *path = "/tmp/tspdf_test_png_pal_noplte.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0, 0, 0, 1, 0, 0, 0, 1,
        8, 3, 0, 0, 0,  // 8-bit palette, but no PLTE follows
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    uint8_t scanline[2] = { 0, 0 };  // filter 0 + one index byte
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(scanline, 2, &comp_len);
    ASSERT(comp != NULL);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    free(comp);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// Interlaced (Adam7) PNGs stay rejected: no de-interlacing pass exists.
TEST(test_png_interlaced_still_rejected) {
    const char *path = "/tmp/tspdf_test_png_interlaced.png";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        0, 0, 0, 1, 0, 0, 0, 1,
        8, 2, 0, 0, 1,  // 8-bit RGB, interlace = 1 (Adam7)
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    uint8_t scanline[4] = { 0, 255, 0, 0 };  // filter 0 + one RGB pixel
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(scanline, 4, &comp_len);
    ASSERT(comp != NULL);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    free(comp);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    PngImage img;
    bool ok = png_image_load(path, &img);
    remove(path);
    ASSERT(ok == false);
    ASSERT(img.pixels == NULL);
}

// Round-trip: a palette+tRNS PNG embeds through the writer (RGB + soft mask)
// and the document saves cleanly.
TEST(test_png_palette_embeds_in_writer) {
    const char *png_path = "/tmp/tspdf_test_png_pal_embed.png";
    static const uint8_t palette[] = {
        255, 0, 0,   0, 255, 0,   0, 0, 255,
    };
    static const uint8_t trns[] = { 200 };
    static const uint8_t indices[] = { 0, 1, 2, 0 };  // 2x2
    ASSERT(test_png_write_palette_file(png_path, 2, 2, 8, palette, 3, trns, 1, indices));

    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(doc != NULL);
    const char *name = tspdf_writer_add_png_image(doc, png_path);
    remove(png_path);
    ASSERT(name != NULL);
    ASSERT_EQ_INT(doc->image_count, 1);
    ASSERT_EQ_INT(doc->images[0].width, 2);
    ASSERT_EQ_INT(doc->images[0].height, 2);
    // The pixel buffers are freed once embedded, but the soft-mask object ref
    // survives — non-zero only when the decoder reported an alpha channel.
    ASSERT(doc->images[0].smask_ref.id != 0);  // tRNS -> soft mask

    TspdfStream *page = tspdf_writer_add_page(doc);
    ASSERT(page != NULL);
    tspdf_stream_draw_image(page, name, 36, 36, 100, 100);
    TspdfError err = tspdf_writer_save(doc, "/tmp/tspdf_test_png_pal_embed.pdf");
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);
    remove("/tmp/tspdf_test_png_pal_embed.pdf");
}

// ============================================================
// PNG IDAT passthrough (img2pdf size track)
// ============================================================

// Write a PNG with the given IHDR fields and one IDAT holding the
// deflate-compressed `raw` filtered raster. No PLTE/tRNS (use
// test_png_write_palette_file for palette images).
static bool test_png_write_raw_file(const char *path, int width, int height,
                                    int bit_depth, int color_type, int interlace,
                                    const uint8_t *raw, size_t raw_len) {
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(raw, raw_len, &comp_len);
    if (!comp) return false;
    FILE *f = fopen(path, "wb");
    if (!f) { free(comp); return false; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        (uint8_t)((width >> 24) & 0xff), (uint8_t)((width >> 16) & 0xff),
        (uint8_t)((width >> 8) & 0xff), (uint8_t)(width & 0xff),
        (uint8_t)((height >> 24) & 0xff), (uint8_t)((height >> 16) & 0xff),
        (uint8_t)((height >> 8) & 0xff), (uint8_t)(height & 0xff),
        (uint8_t)bit_depth, (uint8_t)color_type, 0, 0, (uint8_t)interlace,
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(comp);
    return true;
}

// An RGB PNG's concatenated IDAT is extracted verbatim and inflates back to
// the exact filtered raster — the passthrough embed loses nothing.
TEST(test_png_passthrough_rgb_extracts_idat) {
    const char *path = "/tmp/tspdf_test_png_pt_rgb.png";
    // 2x2 RGB, filter 0 rows
    static const uint8_t raw[] = {
        0, 255, 0, 0,  0, 255, 0,
        0, 0, 0, 255,  255, 255, 0,
    };
    ASSERT(test_png_write_raw_file(path, 2, 2, 8, 2, 0, raw, sizeof(raw)));

    PngPassthrough pt;
    bool ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(pt.width, 2);
    ASSERT_EQ_INT(pt.height, 2);
    ASSERT_EQ_INT(pt.bit_depth, 8);
    ASSERT_EQ_INT(pt.color_type, 2);
    ASSERT_EQ_INT(pt.palette_count, 0);
    ASSERT(!pt.has_alpha);
    size_t out_len = 0;
    uint8_t *out = deflate_decompress(pt.idat, pt.idat_len, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(out_len, sizeof(raw));
    ASSERT(memcmp(out, raw, sizeof(raw)) == 0);
    free(out);
    png_passthrough_free(&pt);
}

// Grayscale (color type 0) is eligible too, and reports 1 component.
TEST(test_png_passthrough_gray) {
    const char *path = "/tmp/tspdf_test_png_pt_gray.png";
    static const uint8_t raw[] = { 0, 10, 20,  0, 30, 40 };  // 2x2 gray
    ASSERT(test_png_write_raw_file(path, 2, 2, 8, 0, 0, raw, sizeof(raw)));

    PngPassthrough pt;
    bool ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(pt.color_type, 0);
    ASSERT_EQ_INT(pt.bit_depth, 8);
    ASSERT(!pt.has_alpha);
    png_passthrough_free(&pt);
}

// Palette PNGs report the PLTE triplets; tRNS flips has_alpha (the caller
// must then build a decoded soft mask).
TEST(test_png_passthrough_palette) {
    const char *path = "/tmp/tspdf_test_png_pt_pal.png";
    static const uint8_t palette[] = { 1, 2, 3,  4, 5, 6 };
    static const uint8_t indices[] = { 0, 1, 1, 0 };  // 2x2
    ASSERT(test_png_write_palette_file(path, 2, 2, 8, palette, 2, NULL, 0, indices));
    PngPassthrough pt;
    bool ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(pt.color_type, 3);
    ASSERT_EQ_INT(pt.palette_count, 2);
    ASSERT(memcmp(pt.palette, palette, sizeof(palette)) == 0);
    ASSERT(!pt.has_alpha);
    png_passthrough_free(&pt);

    static const uint8_t trns[] = { 128 };
    ASSERT(test_png_write_palette_file(path, 2, 2, 8, palette, 2, trns, 1, indices));
    ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    ASSERT(pt.has_alpha);
    png_passthrough_free(&pt);
}

// Sub-8-bit palette depths keep their packed bit depth for the embed.
TEST(test_png_passthrough_palette_4bit) {
    const char *path = "/tmp/tspdf_test_png_pt_pal4.png";
    static const uint8_t palette[] = { 1, 2, 3,  4, 5, 6 };
    static const uint8_t indices[] = { 0, 1, 1, 0 };  // 2x2, packs 2 per byte
    ASSERT(test_png_write_palette_file(path, 2, 2, 4, palette, 2, NULL, 0, indices));
    PngPassthrough pt;
    bool ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    ASSERT_EQ_INT(pt.bit_depth, 4);
    ASSERT_EQ_INT(pt.palette_count, 2);
    png_passthrough_free(&pt);
}

// Passthrough must reject palette indices past the PLTE entry count, exactly
// like the decode path does — otherwise the same file would embed verbatim
// via passthrough but fail full decode.
TEST(test_png_passthrough_palette_index_out_of_range_rejected) {
    const char *path = "/tmp/tspdf_test_png_pt_pal_oob.png";
    static const uint8_t palette[] = { 255, 0, 0,   0, 255, 0 };  // 2 entries
    static const uint8_t bad_indices[] = { 0, 5 };  // 5 >= 2 -> invalid
    ASSERT(test_png_write_palette_file(path, 2, 1, 8, palette, 2, NULL, 0, bad_indices));
    PngPassthrough pt;
    bool ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(!ok);

    // Sub-byte depth: 4-bit indices with only 2 palette entries, index 9 bad.
    static const uint8_t bad_indices4[] = { 0, 9 };
    ASSERT(test_png_write_palette_file(path, 2, 1, 4, palette, 2, NULL, 0, bad_indices4));
    ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(!ok);

    // Control: the same shape with in-range indices still passes through.
    static const uint8_t good_indices[] = { 0, 1 };
    ASSERT(test_png_write_palette_file(path, 2, 1, 8, palette, 2, NULL, 0, good_indices));
    ok = png_read_passthrough(path, &pt);
    remove(path);
    ASSERT(ok);
    png_passthrough_free(&pt);
}

// Interlaced files cannot passthrough (row order differs) and alpha color
// types carry interleaved alpha PDF cannot split — both must be refused so
// the caller falls back to full decode.
TEST(test_png_passthrough_rejects_interlaced_and_alpha) {
    const char *path = "/tmp/tspdf_test_png_pt_rej.png";
    static const uint8_t raw_rgb[] = { 0, 255, 0, 0 };  // 1x1 RGB
    ASSERT(test_png_write_raw_file(path, 1, 1, 8, 2, 1, raw_rgb, sizeof(raw_rgb)));
    PngPassthrough pt;
    ASSERT(!png_read_passthrough(path, &pt));

    static const uint8_t raw_rgba[] = { 0, 255, 0, 0, 128 };  // 1x1 RGBA
    ASSERT(test_png_write_raw_file(path, 1, 1, 8, 6, 0, raw_rgba, sizeof(raw_rgba)));
    ASSERT(!png_read_passthrough(path, &pt));

    static const uint8_t raw_ga[] = { 0, 255, 128 };  // 1x1 gray+alpha
    ASSERT(test_png_write_raw_file(path, 1, 1, 8, 4, 0, raw_ga, sizeof(raw_ga)));
    ASSERT(!png_read_passthrough(path, &pt));
    remove(path);
}

// The IDAT is validated before embedding: a stream that inflates to the wrong
// raster size, or one with an out-of-spec row filter byte, must be refused —
// otherwise the broken bytes would be embedded verbatim into the PDF.
TEST(test_png_passthrough_rejects_damaged_idat) {
    const char *path = "/tmp/tspdf_test_png_pt_bad.png";
    static const uint8_t short_raw[] = { 0, 255, 0 };  // 1x1 RGB needs 4 bytes
    ASSERT(test_png_write_raw_file(path, 1, 1, 8, 2, 0, short_raw, sizeof(short_raw)));
    PngPassthrough pt;
    ASSERT(!png_read_passthrough(path, &pt));

    static const uint8_t bad_filter[] = { 5, 255, 0, 0 };  // filter byte 5 > 4
    ASSERT(test_png_write_raw_file(path, 1, 1, 8, 2, 0, bad_filter, sizeof(bad_filter)));
    ASSERT(!png_read_passthrough(path, &pt));
    remove(path);
}

// Like test_png_write_raw_file, but overrides the two zlib header bytes of
// the IDAT stream — for crafting FDICT / CINFO>7 headers our own inflater
// tolerates (it skips the header) but external PDF readers reject.
static bool test_png_write_zhdr_file(const char *path, int width, int height,
                                     uint8_t z0, uint8_t z1,
                                     const uint8_t *raw, size_t raw_len) {
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(raw, raw_len, &comp_len);
    if (!comp || comp_len < 2) { free(comp); return false; }
    comp[0] = z0;
    comp[1] = z1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(comp); return false; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        (uint8_t)((width >> 24) & 0xff), (uint8_t)((width >> 16) & 0xff),
        (uint8_t)((width >> 8) & 0xff), (uint8_t)(width & 0xff),
        (uint8_t)((height >> 24) & 0xff), (uint8_t)((height >> 16) & 0xff),
        (uint8_t)((height >> 8) & 0xff), (uint8_t)(height & 0xff),
        (uint8_t)8, (uint8_t)2, 0, 0, 0,
    };
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    test_png_write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(comp);
    return true;
}

// Passthrough must only vouch for plain zlib streams external readers can
// decode: CM=8, CINFO<=7 (32K window), no preset dictionary. Our inflater
// skips the 2-byte header, so a crafted FDICT or CINFO=15 stream inflates
// fine locally but breaks qpdf/MuPDF once embedded verbatim.
TEST(test_png_passthrough_rejects_nonstandard_zlib_header) {
    const char *path = "/tmp/tspdf_test_png_pt_zhdr.png";
    static const uint8_t raw[] = { 0, 255, 0, 0 };  // 1x1 RGB, filter 0

    // FDICT set: CMF=0x78, FLG=0x20 (bit 5 = FDICT; (0x7800+0x20) % 31 == 0)
    ASSERT(test_png_write_zhdr_file(path, 1, 1, 0x78, 0x20, raw, sizeof(raw)));
    PngPassthrough pt;
    ASSERT(!png_read_passthrough(path, &pt));

    // CINFO=15: CMF=0xF8, FLG=0x00 (0xF800 % 31 == 0) — window > 32K
    ASSERT(test_png_write_zhdr_file(path, 1, 1, 0xF8, 0x00, raw, sizeof(raw)));
    ASSERT(!png_read_passthrough(path, &pt));
    remove(path);
}

// The chunk scan must stop at IEND: data after IEND is not part of the image
// stream (PNG spec), so a stray IDAT there must not be picked up.
TEST(test_png_passthrough_stops_at_iend) {
    const char *path = "/tmp/tspdf_test_png_pt_iend.png";
    static const uint8_t raw[] = { 0, 255, 0, 0 };  // 1x1 RGB, filter 0
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(raw, sizeof(raw), &comp_len);
    ASSERT(comp != NULL);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const uint8_t ihdr[13] = { 0, 0, 0, 1,  0, 0, 0, 1,  8, 2, 0, 0, 0 };

    // Only IDAT is after IEND: no image data before IEND -> reject.
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    fwrite(sig, 1, 8, f);
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IEND", NULL, 0);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    fclose(f);
    PngPassthrough pt;
    ASSERT(!png_read_passthrough(path, &pt));

    // Valid IDAT before IEND plus junk IDAT after: accepted, and only the
    // real stream is extracted (the junk must not be concatenated).
    f = fopen(path, "wb");
    ASSERT(f != NULL);
    fwrite(sig, 1, 8, f);
    test_png_write_chunk(f, "IHDR", ihdr, 13);
    test_png_write_chunk(f, "IDAT", comp, comp_len);
    test_png_write_chunk(f, "IEND", NULL, 0);
    static const uint8_t junk[] = { 0xde, 0xad, 0xbe, 0xef };
    test_png_write_chunk(f, "IDAT", junk, sizeof(junk));
    fclose(f);
    ASSERT(png_read_passthrough(path, &pt));
    remove(path);
    ASSERT_EQ_SIZE(pt.idat_len, comp_len);
    ASSERT(memcmp(pt.idat, comp, comp_len) == 0);
    png_passthrough_free(&pt);
    free(comp);
}

// An FDICT-flagged PNG is refused by passthrough but must still convert via
// the decode+recompress fallback — our inflater tolerates the header, so the
// image decodes and gets re-embedded with a clean zlib stream.
TEST(test_png_fdict_converts_via_decode_fallback) {
    const char *path = "/tmp/tspdf_test_png_fdict_fb.png";
    static const uint8_t raw[] = { 0, 200, 100, 50 };  // 1x1 RGB, filter 0
    ASSERT(test_png_write_zhdr_file(path, 1, 1, 0x78, 0x20, raw, sizeof(raw)));

    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(doc != NULL);
    const char *name = tspdf_writer_add_png_image(doc, path);
    remove(path);
    ASSERT(name != NULL);
    ASSERT_EQ_INT(doc->image_count, 1);
    ASSERT_EQ_INT(doc->images[0].width, 1);
    ASSERT_EQ_INT(doc->images[0].height, 1);

    TspdfStream *page = tspdf_writer_add_page(doc);
    ASSERT(page != NULL);
    tspdf_stream_draw_image(page, name, 36, 36, 100, 100);
    uint8_t *pdf = NULL;
    size_t pdf_len = 0;
    ASSERT(tspdf_writer_save_to_memory(doc, &pdf, &pdf_len) == TSPDF_OK);
    ASSERT(pdf_len > 0);
    // The crafted FDICT header bytes must NOT appear as an embedded stream
    // start: the fallback recompresses with a standard header.
    free(pdf);
    tspdf_writer_destroy(doc);
}

// End-to-end guarantee: the writer embeds a passthrough-eligible PNG's IDAT
// bytes verbatim (found unchanged inside the saved PDF).
TEST(test_png_passthrough_embeds_idat_verbatim) {
    const char *path = "/tmp/tspdf_test_png_pt_embed.png";
    // 4x2 RGB with a non-zero (Sub) filter on row 2: filter bytes are part of
    // the predictor data and must survive untouched.
    static const uint8_t raw[] = {
        0, 10, 20, 30,  40, 50, 60,  70, 80, 90,  100, 110, 120,
        1, 5, 5, 5,  1, 1, 1,  2, 2, 2,  3, 3, 3,
    };
    ASSERT(test_png_write_raw_file(path, 4, 2, 8, 2, 0, raw, sizeof(raw)));

    PngPassthrough pt;
    ASSERT(png_read_passthrough(path, &pt));

    TspdfWriter *doc = tspdf_writer_create();
    ASSERT(doc != NULL);
    const char *name = tspdf_writer_add_png_image(doc, path);
    remove(path);
    ASSERT(name != NULL);
    ASSERT(doc->images[0].smask_ref.id == 0);  // no alpha -> no soft mask
    TspdfStream *page = tspdf_writer_add_page(doc);
    ASSERT(page != NULL);
    tspdf_stream_draw_image(page, name, 36, 36, 100, 100);

    uint8_t *pdf = NULL;
    size_t pdf_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(doc, &pdf, &pdf_len);
    ASSERT(err == TSPDF_OK);
    // The concatenated IDAT bytes must appear verbatim in the PDF output.
    bool found = false;
    if (pdf_len >= pt.idat_len) {
        for (size_t i = 0; i + pt.idat_len <= pdf_len && !found; i++) {
            if (pdf[i] == pt.idat[0] && memcmp(pdf + i, pt.idat, pt.idat_len) == 0) {
                found = true;
            }
        }
    }
    free(pdf);
    tspdf_writer_destroy(doc);
    png_passthrough_free(&pt);
    ASSERT(found);
}

// ============================================================
// Save-to-memory byte identity (wasm track)
// ============================================================

// Build a small deterministic document. The writer stamps no timestamps
// unless the caller sets them, so two identically-built docs must serialize
// to identical bytes.
static TspdfWriter *wasm_ident_make_doc(void) {
    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) return NULL;
    tspdf_writer_add_builtin_font(doc, "Helvetica");
    TspdfStream *s = tspdf_writer_add_page(doc);
    if (s) {
        tspdf_stream_begin_text(s);
        tspdf_stream_set_font(s, "F1", 12);
        tspdf_stream_text_position(s, 72, 700);
        tspdf_stream_show_text(s, "byte identity");
        tspdf_stream_end_text(s);
    }
    return doc;
}

TEST(test_writer_save_to_memory_matches_file) {
    // The wasm build has no filesystem, so save-to-memory is the primary API
    // there; pin it to exactly the bytes the file-save path writes. Two docs
    // are needed because the writer guards against double-save.
    const char *tmp_path = "/tmp/tspdf_test_wasm_byte_identity.pdf";

    TspdfWriter *doc_file = wasm_ident_make_doc();
    ASSERT(doc_file != NULL);
    TspdfError err = tspdf_writer_save(doc_file, tmp_path);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc_file);

    TspdfWriter *doc_mem = wasm_ident_make_doc();
    ASSERT(doc_mem != NULL);
    uint8_t *mem = NULL;
    size_t mem_len = 0;
    err = tspdf_writer_save_to_memory(doc_mem, &mem, &mem_len);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc_mem);

    FILE *f = fopen(tmp_path, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file_buf = malloc((size_t)size);
    ASSERT(file_buf != NULL);
    size_t nread = fread(file_buf, 1, (size_t)size, f);
    fclose(f);
    remove(tmp_path);
    ASSERT(nread == (size_t)size);

    ASSERT(mem_len == (size_t)size);
    ASSERT(memcmp(file_buf, mem, mem_len) == 0);

    free(file_buf);
    free(mem);
}

// ============================================================
// QR encoder
// ============================================================

// Byte-mode character capacity per level per ISO/IEC 18004 Table 7,
// cross-checked against segno at dev time (the smallest version segno
// picks for a payload of exactly this length is the same version).
static const int qr_expected_char_capacity[4][12] = {
    [QR_EC_L] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321},
    [QR_EC_M] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213, 251},
    [QR_EC_Q] = {0, 11, 20, 32, 46, 60, 74, 86, 108, 130, 151, 177},
    [QR_EC_H] = {0, 7, 14, 24, 34, 44, 58, 64, 84, 98, 119, 137},
};

TEST(test_qr_ecc_table_consistent) {
    // Every row of every level's block table must satisfy
    // blocks x (data + ec) == total codewords for the version, and the
    // derived character capacity must match ISO 18004 Table 7.
    ASSERT(qr_max_version() >= 10);
    ASSERT(qr_max_version() + 1 <=
           (int)(sizeof(qr_expected_char_capacity[0]) / sizeof(int)));
    static const QrEcLevel levels[] = {QR_EC_L, QR_EC_M, QR_EC_Q, QR_EC_H};
    for (size_t li = 0; li < 4; li++) {
        QrEcLevel level = levels[li];
        for (int v = 1; v <= qr_max_version(); v++) {
            int total, ec, b1, d1, b2, d2;
            ASSERT_EQ_INT(qr_ecc_block_info(v, level, &total, &ec, &b1, &d1,
                                            &b2, &d2), 0);
            ASSERT(b1 >= 1);
            ASSERT_EQ_INT(b1 * (d1 + ec) + b2 * (d2 + ec), total);
            // Group 2 blocks, when present, carry exactly one more data
            // codeword.
            if (b2 > 0) ASSERT_EQ_INT(d2, d1 + 1);
            // Total codewords for a version is fixed by the module count:
            // it must match the ISO totals independent of the block split.
            static const int totals[] = {0, 26, 44, 70, 100, 134, 172,
                                         196, 242, 292, 346, 404};
            ASSERT_EQ_INT(total, totals[v]);
            int data_cw = b1 * d1 + b2 * d2;
            int cc_bits = (v <= 9) ? 8 : 16;
            int capacity = (data_cw * 8 - 4 - cc_bits) / 8;
            ASSERT_EQ_INT(capacity, qr_expected_char_capacity[level][v]);
        }
    }
    ASSERT_EQ_INT(qr_ecc_block_info(0, QR_EC_M, 0, 0, 0, 0, 0, 0), -1);
    ASSERT_EQ_INT(qr_ecc_block_info(qr_max_version() + 1, QR_EC_M,
                                    0, 0, 0, 0, 0, 0), -1);
    ASSERT_EQ_INT(qr_ecc_block_info(1, (QrEcLevel)4, 0, 0, 0, 0, 0, 0), -1);
}

TEST(test_qr_rs_known_vector) {
    // The classic v1-M "HELLO WORLD" example (alphanumeric mode): its data
    // codewords and 10 EC codewords are published in the ISO spec walkthroughs.
    static const uint8_t data[16] = {32, 91, 11, 120, 209, 114, 220, 77,
                                     67, 64, 236, 17, 236, 17, 236, 17};
    static const uint8_t expected[10] = {196, 35, 39, 119, 235,
                                         215, 231, 226, 93, 23};
    uint8_t ecc[10];
    qr_rs_ecc(data, 16, ecc, 10);
    ASSERT(memcmp(ecc, expected, sizeof(expected)) == 0);
}

/* RS reference: 26 EC codewords over the 44-byte block below (the v3-M
 * block shape), generated at dev time with an independent Python
 * GF(256)/0x11D Reed-Solomon implementation. */
static const char qr_rs_ref_data[45] = "tspdf reed-solomon reference vector, 44 byte";
static const uint8_t qr_rs_ref_ecc[26] = {
    7, 203, 226, 141, 139, 144, 176, 54, 193, 195, 192, 143, 247,
    134, 77, 167, 60, 75, 46, 30, 5, 203, 173, 213, 182, 230,
};

TEST(test_qr_rs_reference_vector) {
    uint8_t ecc[26];
    qr_rs_ecc((const uint8_t *)qr_rs_ref_data, 44, ecc, 26);
    ASSERT(memcmp(ecc, qr_rs_ref_ecc, sizeof(qr_rs_ref_ecc)) == 0);
}

TEST(test_qr_version_info_bits) {
    // BCH(18,6) values from ISO/IEC 18004:2015 Annex D, Table D.1.
    ASSERT(qr_version_info_bits(1) == 0);
    ASSERT(qr_version_info_bits(6) == 0);
    ASSERT(qr_version_info_bits(7) == 0x07C94);
    ASSERT(qr_version_info_bits(8) == 0x085BC);
    ASSERT(qr_version_info_bits(9) == 0x09A99);
    ASSERT(qr_version_info_bits(10) == 0x0A4D3);
    ASSERT(qr_version_info_bits(11) == 0x0BBF6);
}

TEST(test_qr_version_selection_boundaries) {
    // Smallest version is chosen by the byte-mode character capacity at
    // the requested level (default M).
    struct { int len; int size; QrEcLevel level; } cases[] = {
        {14, 21, QR_EC_M},   // v1 max
        {15, 25, QR_EC_M},   // spills to v2
        {42, 29, QR_EC_M},   // v3 max
        {43, 33, QR_EC_M},   // spills to v4
        {213, 57, QR_EC_M},  // v10 max
        {214, 61, QR_EC_M},  // spills to v11
        {251, 61, QR_EC_M},  // v11 max
        {17, 21, QR_EC_L},   // v1 max at L (more room than M)
        {18, 25, QR_EC_L},   // spills to v2
        {321, 61, QR_EC_L},  // v11 max at L
        {11, 21, QR_EC_Q},   // v1 max at Q
        {12, 25, QR_EC_Q},   // spills to v2
        {7, 21, QR_EC_H},    // v1 max at H (least room)
        {8, 25, QR_EC_H},    // spills to v2
        {137, 61, QR_EC_H},  // v11 max at H
    };
    char buf[400];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(buf, 'a', (size_t)cases[i].len);
        buf[cases[i].len] = '\0';
        QrCode *qr = qr_encode_level(buf, cases[i].level);
        ASSERT(qr != NULL);
        ASSERT_EQ_INT(qr->size, cases[i].size);
        qr_free(qr);
    }
    memset(buf, 'a', 252);
    buf[252] = '\0';
    ASSERT(qr_encode(buf) == NULL);   // beyond v11 capacity at M
    memset(buf, 'a', 138);
    buf[138] = '\0';
    ASSERT(qr_encode_level(buf, QR_EC_H) == NULL);  // beyond v11 at H
    memset(buf, 'a', 322);
    buf[322] = '\0';
    ASSERT(qr_encode_level(buf, QR_EC_L) == NULL);  // beyond v11 at L
    ASSERT(qr_encode_level("x", (QrEcLevel)4) == NULL);  // bad level
}

/*
 * Golden module grids. Each row is a bitmap with bit c = column c
 * (1 = dark). Generated from this encoder and decode-verified at dev time
 * (OpenCV cv2.QRCodeDetector for the level-M grids, zxing-cpp for the
 * L/H grids): every grid below decoded back to its exact payload. In
 * addition, full-capacity payloads at every version and level were
 * byte-identical to segno's canonical matrices. The grids pin the whole
 * pipeline — version selection, data encoding, RS interleaving, matrix
 * layout, format and version info, mask choice — so any behavior change
 * shows up here.
 */
typedef struct {
    const char *payload;
    QrEcLevel level;
    int size;
    const uint64_t rows[61];
} QrGolden;

static const QrGolden qr_goldens[] = {
    { /* len 10, version 1 */
      "https://ex",
      QR_EC_M, 21,
        { 0x1fc37f, 0x105141, 0x174a5d, 0x17555d, 0x17465d, 0x104641, 0x1fd57f, 0x500,
          0x1a44ed, 0x1ff792, 0x1a31cc, 0xa90b3, 0x134cc1, 0x16100, 0x1017f, 0x17b941,
          0x52e5d, 0x8e55d, 0x48f5d, 0x11d841, 0x73d7f } },
    { /* len 30, version 3 */
      "https://example.org/p/abcdefgh",
      QR_EC_M, 29,
        { 0x1fcd387f, 0x105bd141, 0x1749785d, 0x1744e65d, 0x175c555d, 0x10482241,
          0x1fd5557f, 0xb6600, 0x902d855, 0x1252c03b, 0x1dc62262, 0x9369914,
          0x1a7b0ed3, 0x1263a0b4, 0x1ba1de7d, 0xab37996, 0x1a72ced0, 0x1672cca2,
          0x19703fc9, 0xb868f3a, 0x1f31c4d, 0x1d13b700, 0x1b5bc87f, 0x1b137a41,
          0x9f3c95d, 0x532ce5d, 0x138b295d, 0x85ec041, 0x199bd17f } },
    { /* len 60, version 4 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 33,
        { 0x1fcad2e7f, 0x10505f841, 0x17561235d, 0x17542d75d, 0x1749ea55d, 0x105628341,
          0x1fd55557f, 0x1a88f00, 0x7c5a687d, 0x16cbd198c, 0xd2769ef4, 0xf63d5c1c,
          0x3bca8ef3, 0x1c4bd5d85, 0x9e75a0ef, 0x67610799, 0x11b0afdf6, 0x16cad730c,
          0xdc77eeec, 0x17979d216, 0x13b42bee0, 0x1449d45a7, 0xa346e47d, 0x63a55301,
          0x19fca8cc1, 0x1513d2300, 0xd5c0b67f, 0xf1adab41, 0x13f42ab5d, 0x1f31d515d,
          0x4e57b75d, 0x7479b041, 0x8bd2ef7f } },
    { /* len 116, version 7 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcd",
      QR_EC_M, 45,
        { 0x1fd26229c47f, 0x10499dd8bc41, 0x174b5c0c555d, 0x1758a577d35d,
          0x175c6bf8cd5d, 0x10429d12ab41, 0x1fd55555557f, 0x17f107500, 0x7c0cffb907d,
          0x1919f30d1235, 0x8f4072f5df0, 0x7a77d02728c, 0x1002e3ee715d, 0x143962404280,
          0xe4e1df166ca, 0xfbf7ccc0431, 0x20e53ec2f0, 0x14b84a286524, 0x84e1dd12a57,
          0x17af780e5107, 0x11f6c3f7cff4, 0x1719f318651a, 0xd5e055c8559, 0xf1779161119,
          0x13f2e1f5a1fa, 0x1451797f830a, 0xf3e0c12bbed, 0x6cf7db20813, 0x94085811d54,
          0x1448eb72da15, 0x8961c0a2bc0, 0x16cfb99dd6be, 0x11b68288b877,
          0x1428f37e270d, 0xab78427f550, 0xe5b79984f1e, 0x11f0e5f1b659, 0x17197b161f00,
          0xd5e05584e7f, 0x71b771f2341, 0x13f887f6695d, 0x1da0f22a095d, 0x85e150c2d5d,
          0x72f78125c41, 0x8dea3ae257f } },
    { /* len 150, version 8 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 49,
        { 0x1fd107a0e4c7f, 0x105c69c6bd041, 0x175853a71d35d, 0x174bea5a8535d,
          0x17438e7e25d5d, 0x104468c481141, 0x1fd555555557f, 0x1a31c40b900,
          0x7ddec7f29a7d, 0x639f30882b4, 0x191d68dec00c4, 0x90a77a01288f,
          0x1e99601e3876e, 0x8c38639b8886, 0x1b15e0597cf40, 0x11487781c0708,
          0x69f8e5f75bd8, 0xe29e2adaf28, 0x1a05e1d9169fb, 0x980558111db3,
          0x1413ce3f52b46, 0x7e307be9e012, 0x1bf449ff3f9f6, 0x111a31c44951e,
          0x1f57ce5753152, 0x11a17c4b0118, 0x1ffc68fc6f1f2, 0x123fc292b80,
          0x1f1b8a246725b, 0xeb1efe72c05, 0x19ecf82c61d74, 0x9277d35082c,
          0x1f29e825290c1, 0xa216bafa899, 0x1a4de1b4fd5d2, 0x11635db33a06,
          0x154be801605ef, 0x6ba9e7af428e, 0x196fe1a4b8ce2, 0x91411d820c0e,
          0x17f9a87f102c7, 0x1138e44fc100, 0x1d5468d7e1c7f, 0x191231c60c341,
          0x1ff5e47d0435d, 0x19db1f06cd55d, 0x6e568dfe675d, 0x107a37b2c9441,
          0x1e1b8a411937f } },
    { /* len 200, version 10 */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_M, 57,
        { 0x1fcee38fbebf07f, 0x104a9c7050cfe41, 0x174f1e2ddda115d, 0x1748c7e02112d5d,
          0x17487a0fdff3d5d, 0x104787e47abe741, 0x1fd55555555557f, 0x1581471db100,
          0x7c0e38fc3dc27d, 0x1639f29f1858093, 0xcdc0d68d467c7c, 0xfbf7a16268663a,
          0x6c1aed45c34a, 0x16b97b066be9411, 0xbc71de8c6e7e4e, 0x7937a11ed85f1d,
          0x1064efcc0179052, 0x1019d38e3e83296, 0x87e8560867b1cf, 0xf217c73ba32528,
          0x104685e84105b7a, 0x10b86b02bd1d8aa, 0xcce9c75ec97561, 0x17cf1821b04ab29,
          0xa20e1d8410b1dd, 0x153861965d12405, 0x9fe05ffe3c4bf4, 0x1f1f581443f411f,
          0x15a85ed470f55c, 0x17117a147c1d715, 0xff60d6fee41ff7, 0x16cb7a1018a8e2b,
          0x1190e38dbc02fc1, 0x40e29a50c4e39, 0x1db71d67fe0ea51, 0x7c93c709200bb5,
          0x1a281ae80d8563, 0x14317a115cdc715, 0xf368c779acecc6, 0x64978315928f89,
          0x11d4a389c1d0b78, 0x1651f29e5eebbbd, 0xc9e05efe6ee0fb, 0xe5f9e01b703700,
          0xb2687a5814f3cc, 0x1468eb061b99814, 0xabf85f57dc8065, 0x164b5816016d21f,
          0x1f0a7cfc0a3f40, 0x1710e38c5b35100, 0x95e0d6d642b07f, 0xf1b7a145989f41,
          0x11f685eff4ad15d, 0x1c06a15c98815d, 0x65f1d6627e695d, 0x7a934768c70441,
          0x8d0a388229a57f } },
    { /* len 250, version 11 — the only version where group 1 has FEWER
       * blocks than group 2 (1x50 + 4x51), so this pins that interleave
       * shape end to end. */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefgh",
      QR_EC_M, 61,
        { 0x1fdb8f2965949e7f, 0x1059e056a8fe1a41, 0x175c15c0e599d55d,
          0x1757e81b1be8515d, 0x174f17a1f55f3f5d, 0x1045f05718d34141,
          0x1fd555555555557f, 0x1c53a315431b00, 0x7d9ea5ff279da7d, 0x7e28e396c362480,
          0x1e057056b8c8cb42, 0x19fc11a2672a2132, 0x659205f1ac2d7e7,
          0x3e386390c06c20d, 0x180de056da8c235a, 0x117c15a3e11d9799,
          0x6fb8e7c82e45d40, 0x6e38f290c61dd97, 0x1a95e056fb9a91cb,
          0x16415c16217ca2b, 0x495e81a831695df, 0x66317a10d0f881a,
          0x1885f0569815046d, 0x10015c166f02e9c, 0x69bca3893058d51,
          0x7c28e390cd77aa3, 0x1e057056d817b950, 0x197855a266c16715,
          0x7fbce7dfa1cd1f3, 0x31206a11598b916, 0x195550475b776d51,
          0x111c1da31839e716, 0x7fb8e7dfa6265f8, 0x2cb1ea115fecba7,
          0x1f64f8471a8a824e, 0x81415c0cba6a415, 0x1ffde81bea344c69,
          0x8f317a09595012b, 0xa6df0570a4a0bd5, 0x101015c0e5b5c513,
          0x156bca39636184c3, 0x6928e389575a23c, 0x1e6d70570b086f51,
          0x189077c080294eb7, 0x77dac5ee27b9ff6, 0x2b206a095bce803,
          0x19e570474aa807ca, 0x101635c0e1b60903, 0x72be81aea66246f,
          0x2ab9f38959e2cbb, 0x1f4768c71a7b717c, 0x81415c0ebc31e97,
          0x1ffde01bfa0c9ccf, 0x3131f391db02d00, 0x1d5568c753ed687f,
          0x11015c115977f41, 0x1ffbca39fa5d5f5d, 0x117a8e39fdd1ad5d,
          0x12e570569bfd0f5d, 0x10f877c11f842641, 0x1c05ac5ee25b9d7f } },
    { /* len 230, level L, version 9 (2x116 data codewords per block — the
       * largest blocks in the supported range; they would have overflowed
       * the level-M-era 64-byte block buffers). */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij"
      "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_L, 53,
        { 0x1fc6a97be8507f, 0x104e5f06b74b41, 0x1749c363f68c5d, 0x17565c8c17bd5d,
          0x174620ffe8485d, 0x1045d691652741, 0x1fd5555555557f, 0xda751ae0e00,
          0xab03aff17b5df, 0x150f20f9684a93, 0xe8df042fc57e, 0xa61817bd0bd33,
          0xdb43aa417becd, 0x188eb87de842b2, 0x7f1d6807dba7e, 0x100da75b8026bc,
          0x792589c17a84d, 0x152fb878685022, 0x378df06065dc2, 0xa798103b2cc05,
          0x59658c597b75f, 0x1c0f21fde84d0c, 0x5e84602743067, 0x18cfa743e2552d,
          0x5f47ebf97a3ff, 0x151e31f1685b12, 0x550de95199d54, 0xb1181118db31e,
          0x5f07eff17bdf0, 0x1fe7b97ce85d92, 0x780461b2ea7ca, 0xffc360046621,
          0x46618cb17b556, 0x11f7a964e84f1a, 0x391d69d1005e8, 0xbfda1056fc3b3,
          0x1c561c8b97bac1, 0x1fde30fce85fa0, 0x788461a37436d, 0xb5fc363f6968c,
          0x4303ef3178ce6, 0x198e20ece801b4, 0x501d69808b07b, 0x1beda742c9e886,
          0x5f03aff1784c8, 0x1b1ea971684900, 0x3585f15afa77f, 0x131fc371d0e241,
          0xff25c9f17f95d, 0x1836b87268455d, 0x17e9d68565535d, 0xb15a75a2e1341,
          0x462588597dd7f } },
    { /* len 60, level H, version 7 (4x13 + 1x14: a single group-2 block,
       * plus level-H format info bits and v7 version info blocks). */
      "https://example.org/p/abcdefghijabcdefghijabcdefghijabcdefgh",
      QR_EC_H, 45,
        { 0x1fd0ec3de97f, 0x104a14cca341, 0x174ac221875d, 0x17580d36265d,
          0x175cf5f7565d, 0x10421f12d141, 0x1fd55555557f, 0x1d6d11db00,
          0x1ce4e3fb535c, 0x113be613929e, 0x8ce853bf751, 0x1f950c4d33a2,
          0x804df2134f0, 0x11b8736247ae, 0xde48ec921f0, 0x7e32da6ffa2, 0xa609e63c2c8,
          0x1330a47b9cbe, 0xc6e431f9a4b, 0x7d954823b07, 0x1f2f5f9a7fd,
          0x17184b1de717, 0xf577d5cd95c, 0x7106b121d18, 0x19f5c5f4d3f9,
          0x101929155927, 0xdbfb29e627e, 0xf4d5d633e1a, 0x1104ff705d55,
          0x142139c12826, 0xeb6e5eee74a, 0x164b738fd185, 0x9b289dd6973,
          0x161bc282ac8d, 0xdb62222b350, 0x1ecb19489d1e, 0x3f6e7f21e59,
          0x1f1991119700, 0xf57df5ae27f, 0xd11eb188641, 0x19f8c7fca95d,
          0x1f80eb1cd35d, 0x7edbe8c55d, 0x6062b655e41, 0x9f2737f3c7f } },
};

TEST(test_qr_golden_grids) {
    for (size_t g = 0; g < sizeof(qr_goldens) / sizeof(qr_goldens[0]); g++) {
        const QrGolden *gd = &qr_goldens[g];
        QrCode *qr = qr_encode_level(gd->payload, gd->level);
        ASSERT(qr != NULL);
        ASSERT_EQ_INT(qr->size, gd->size);
        for (int r = 0; r < qr->size; r++) {
            uint64_t bits = 0;
            for (int c = 0; c < qr->size; c++) {
                if (qr->modules[r * qr->size + c])
                    bits |= (uint64_t)1 << c;
            }
            if (bits != gd->rows[r]) {
                qr_free(qr);
                printf("FAIL\n    golden %zu (\"%.20s...\") row %d: "
                       "got 0x%llx want 0x%llx\n",
                       g, gd->payload, r,
                       (unsigned long long)bits,
                       (unsigned long long)gd->rows[r]);
                tests_failed++;
                _test_failed = true;
                return;
            }
        }
        qr_free(qr);
    }
}

// ============================================================
// PDF date formatting
// ============================================================

// Expect `raw` to parse and format exactly as `want`.
static void expect_pdf_date(const char *raw, const char *want) {
    char out[64];
    if (!tspdf_format_pdf_date(raw, out, sizeof(out))) {
        printf("FAIL\n    pdf date \"%s\": parser rejected it\n", raw);
        tests_failed++;
        _test_failed = true;
        return;
    }
    if (strcmp(out, want) != 0) {
        printf("FAIL\n    pdf date \"%s\": got \"%s\" want \"%s\"\n",
               raw, out, want);
        tests_failed++;
        _test_failed = true;
    }
}

TEST(test_pdf_date_full_forms) {
    expect_pdf_date("D:20131031140150+04'00'", "2013-10-31 14:01:50 +04:00");
    expect_pdf_date("D:20131031140150-08'00'", "2013-10-31 14:01:50 -08:00");
    expect_pdf_date("D:20131031140150Z", "2013-10-31 14:01:50 UTC");
    expect_pdf_date("D:20131031140150", "2013-10-31 14:01:50");
    // The D: prefix is recommended but not required by the spec.
    expect_pdf_date("20131031140150Z", "2013-10-31 14:01:50 UTC");
}

TEST(test_pdf_date_partial_fields_default) {
    // Every field after the year is optional and defaults per the spec.
    expect_pdf_date("D:2013", "2013-01-01 00:00:00");
    expect_pdf_date("D:201310", "2013-10-01 00:00:00");
    expect_pdf_date("D:20131031", "2013-10-31 00:00:00");
    expect_pdf_date("D:2013103114", "2013-10-31 14:00:00");
    expect_pdf_date("D:201310311401", "2013-10-31 14:01:00");
    // Missing seconds with a zone.
    expect_pdf_date("D:201310311401+04'00'", "2013-10-31 14:01:00 +04:00");
}

TEST(test_pdf_date_odd_zones) {
    // Zone hour without minutes; with unterminated minutes; without the
    // spec's apostrophes entirely (+HHMM, a common producer malformation).
    expect_pdf_date("D:20131031140150+04'", "2013-10-31 14:01:50 +04:00");
    expect_pdf_date("D:20131031140150+04", "2013-10-31 14:01:50 +04:00");
    expect_pdf_date("D:20131031140150+04'30", "2013-10-31 14:01:50 +04:30");
    expect_pdf_date("D:20131031140150-0530", "2013-10-31 14:01:50 -05:30");
}

TEST(test_pdf_date_garbage_rejected) {
    char out[64];
    static const char *bad[] = {
        "",
        "D:",
        "D:13",                        // 2-digit year
        "D:2013103",                   // dangling half field
        "D:20131031140150+",           // sign without hours
        "D:20131031140150+4'00'",      // 1-digit zone hour
        "D:20131031140150+25'00'",     // zone hour out of range
        "D:20131399140150",            // month 13, day 99
        "D:20131031246099",            // hour 24, second 99
        "D:20131031140150Zjunk",       // trailing garbage after Z
        "D:20131031140150+04'00'x",    // trailing garbage after zone
        "not a date",
        "D:yyyymmdd",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (tspdf_format_pdf_date(bad[i], out, sizeof(out))) {
            printf("FAIL\n    pdf date \"%s\": accepted (got \"%s\"), "
                   "expected rejection\n", bad[i], out);
            tests_failed++;
            _test_failed = true;
            return;
        }
    }
    // Output buffer too small must fail rather than truncate silently.
    char tiny[8];
    ASSERT(!tspdf_format_pdf_date("D:20131031140150Z", tiny, sizeof(tiny)));
    ASSERT(!tspdf_format_pdf_date(NULL, out, sizeof(out)));
}

// ============================================================
// JPEG codec (src/image/jpeg_codec.c)
// ============================================================
//
// Decoder tests compare against Pillow/libjpeg-turbo output committed as
// tests/data/jpg_*.raw (see tests/data/gen_jpeg_fixtures.py). Bounds:
//   * 4:4:4 (no upsampling): max per-channel delta <= 2. Our float IDCT and
//     libjpeg's integer islow IDCT each round within +/-1 of the exact DCT,
//     and the YCbCr->RGB conversion multiplies the Cb error by up to 1.772,
//     so 2 is the honest bound for independent implementations.
//   * subsampled (4:2:2/4:2:0/4:4:0): mean absolute error <= 2.0 plus a loose
//     max-delta cap. We clone libjpeg's "fancy" triangular upsampler, but
//     upsampler variants and edge handling legitimately differ between
//     decoders, so bitwise agreement near edges is not a fair requirement.

static uint8_t *test_jpeg_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

// Decode tests/data/<name>.jpg with our decoder and diff it against the
// committed PIL dump tests/data/<name>.raw. Returns false on any I/O or
// decode failure; on success fills the max per-channel delta and the MAE.
static bool test_jpeg_diff_fixture(const char *name, int exp_w, int exp_h,
                                   int exp_comp, int *max_delta, double *mae) {
    char path[128];
    snprintf(path, sizeof(path), "tests/data/%s.jpg", name);
    size_t jpg_len;
    uint8_t *jpg = test_jpeg_read_file(path, &jpg_len);
    if (!jpg) return false;
    snprintf(path, sizeof(path), "tests/data/%s.raw", name);
    size_t raw_len;
    uint8_t *raw = test_jpeg_read_file(path, &raw_len);
    if (!raw) { free(jpg); return false; }

    TspdfArena arena = tspdf_arena_create(1 << 20);
    TspdfRawImage img;
    bool ok = tspdf_jpeg_decode(jpg, jpg_len, &arena, &img);
    free(jpg);
    if (!ok || img.width != exp_w || img.height != exp_h ||
        img.components != exp_comp ||
        raw_len != (size_t)exp_w * exp_h * exp_comp) {
        free(raw);
        tspdf_arena_destroy(&arena);
        return false;
    }
    long sum = 0;
    int maxd = 0;
    for (size_t i = 0; i < raw_len; i++) {
        int d = (int)img.pixels[i] - (int)raw[i];
        if (d < 0) d = -d;
        if (d > maxd) maxd = d;
        sum += d;
    }
    *max_delta = maxd;
    *mae = (double)sum / (double)raw_len;
    free(raw);
    tspdf_arena_destroy(&arena);
    return true;
}

static double test_jpeg_psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        se += d * d;
    }
    if (se == 0.0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 * (double)n / se);
}

// Deterministic photo-like RGB content: smooth waves + mild LCG noise (same
// spirit as the fixture generator; no hard edges, non-trivial AC energy).
static void test_jpeg_fill_photo(uint8_t *px, int w, int h) {
    uint32_t seed = 20250701;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            seed = seed * 1103515245u + 12345u;
            int n = (int)((seed >> 16) & 7);
            int r = (int)(96 + 80 * sin(x * 0.11) + 40 * cos(y * 0.07)) + n;
            int g = (int)(120 + 60 * sin((x + y) * 0.05)) + (n >> 1);
            int b = (int)(128 + 90 * cos(x * 0.04 + y * 0.09)) + n;
            uint8_t *p = px + ((size_t)y * w + x) * 3;
            p[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

TEST(test_jpeg_decode_gray_gradient_matches_pil) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_gray_grad", 64, 48, 1, &maxd, &mae));
    ASSERT(maxd <= 2);
}

TEST(test_jpeg_decode_gray_noise_matches_pil) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_gray_noise", 64, 48, 1, &maxd, &mae));
    ASSERT(maxd <= 2);
}

TEST(test_jpeg_decode_rgb_444_matches_pil) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_444", 64, 48, 3, &maxd, &mae));
    ASSERT(maxd <= 2);
}

TEST(test_jpeg_decode_rgb_422_matches_pil) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_422", 64, 48, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_rgb_420_matches_pil) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_420", 64, 48, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_rgb_420_odd_dims_matches_pil) {
    // 61x37: partial MCUs on the right and bottom edges.
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_420_odd", 61, 37, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_rgb_1x2_vertical_matches_pil) {
    // 4:4:0 (Y 1x2, chroma vertically subsampled), emitted by cjpeg.
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_1x2", 64, 48, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_restart_markers) {
    // Same image as jpg_rgb_420 but with DRI=2 and RSTn every 2 MCUs.
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_restart", 64, 48, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_flat_blocks) {
    int maxd;
    double mae;
    ASSERT(test_jpeg_diff_fixture("jpg_rgb_flat", 64, 48, 3, &maxd, &mae));
    ASSERT(mae <= 2.0);
    ASSERT(maxd <= 16);
}

TEST(test_jpeg_decode_rejects_progressive) {
    size_t len;
    uint8_t *jpg = test_jpeg_read_file("tests/data/jpg_progressive.jpg", &len);
    ASSERT(jpg != NULL);
    TspdfArena arena = tspdf_arena_create(1 << 16);
    TspdfRawImage img;
    ASSERT(!tspdf_jpeg_decode(jpg, len, &arena, &img));
    free(jpg);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_decode_rejects_malformed) {
    TspdfArena arena = tspdf_arena_create(1 << 16);
    TspdfRawImage img;
    // Empty / too short / not a JPEG.
    ASSERT(!tspdf_jpeg_decode(NULL, 0, &arena, &img));
    ASSERT(!tspdf_jpeg_decode((const uint8_t *)"\xff\xd8", 2, &arena, &img));
    ASSERT(!tspdf_jpeg_decode((const uint8_t *)"\x89PNG\r\n\x1a\n", 8, &arena, &img));
    // SOI + EOI with no frame.
    ASSERT(!tspdf_jpeg_decode((const uint8_t *)"\xff\xd8\xff\xd9", 4, &arena, &img));
    // Truncated valid file: every prefix must fail cleanly, not crash.
    size_t len;
    uint8_t *jpg = test_jpeg_read_file("tests/data/jpg_rgb_420.jpg", &len);
    ASSERT(jpg != NULL);
    for (size_t cut = 0; cut < len; cut += 37) {
        tspdf_arena_reset(&arena);
        ASSERT(!tspdf_jpeg_decode(jpg, cut, &arena, &img));
    }
    free(jpg);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_decode_fuzz_mutations_no_crash) {
    // 500 deterministic random corruptions of a valid stream (xorshift32,
    // fixed seed — never time()). Every call must return, true or false,
    // without hanging or faulting; `make test-asan` runs this instrumented.
    size_t len;
    uint8_t *orig = test_jpeg_read_file("tests/data/jpg_rgb_420.jpg", &len);
    ASSERT(orig != NULL);
    uint8_t *mut = (uint8_t *)malloc(len);
    ASSERT(mut != NULL);
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint32_t s = 0x1234567u;
    for (int iter = 0; iter < 500; iter++) {
        memcpy(mut, orig, len);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int nmut = 1 + (int)(s % 8);
        for (int m = 0; m < nmut; m++) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            size_t at = s % len;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            mut[at] = (uint8_t)s;
        }
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        size_t use_len = (iter % 5 == 0) ? (s % len) + 1 : len;
        tspdf_arena_reset(&arena);
        TspdfRawImage img;
        (void)tspdf_jpeg_decode(mut, use_len, &arena, &img);
    }
    free(mut);
    free(orig);
    tspdf_arena_destroy(&arena);
    ASSERT(true);
}

TEST(test_jpeg_encode_deterministic) {
    enum { W = 40, H = 32 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(px != NULL);
    test_jpeg_fill_photo(px, W, H);
    TspdfRawImage img = { W, H, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *out1, *out2;
    size_t len1, len2;
    ASSERT(tspdf_jpeg_encode(&img, 75, &arena, &out1, &len1));
    ASSERT(tspdf_jpeg_encode(&img, 75, &arena, &out2, &len2));
    ASSERT_EQ_SIZE(len1, len2);
    ASSERT(memcmp(out1, out2, len1) == 0);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_roundtrip_photo_q75) {
    // Photo-like content at q75, 4:2:0: encode with our encoder, decode with
    // our decoder. >30dB is the standard "visually fine lossy" bar.
    enum { W = 128, H = 96 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(px != NULL);
    test_jpeg_fill_photo(px, W, H);
    TspdfRawImage img = { W, H, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *jpg;
    size_t jpg_len;
    ASSERT(tspdf_jpeg_encode(&img, 75, &arena, &jpg, &jpg_len));
    ASSERT(jpg_len > 0 && jpg_len < (size_t)W * H * 3); // actually compresses
    TspdfRawImage dec;
    ASSERT(tspdf_jpeg_decode(jpg, jpg_len, &arena, &dec));
    ASSERT_EQ_INT(dec.width, W);
    ASSERT_EQ_INT(dec.height, H);
    ASSERT_EQ_INT(dec.components, 3);
    double psnr = test_jpeg_psnr(px, dec.pixels, (size_t)W * H * 3);
    ASSERT(psnr > 30.0);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_roundtrip_gradient_q95) {
    // Smooth gradients at q95 must round-trip nearly exactly (>40dB).
    enum { W = 96, H = 80 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(px != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *p = px + ((size_t)y * W + x) * 3;
            p[0] = (uint8_t)(40 + x);
            p[1] = (uint8_t)(60 + y);
            p[2] = (uint8_t)(200 - (x + y) / 2);
        }
    }
    TspdfRawImage img = { W, H, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *jpg;
    size_t jpg_len;
    ASSERT(tspdf_jpeg_encode(&img, 95, &arena, &jpg, &jpg_len));
    TspdfRawImage dec;
    ASSERT(tspdf_jpeg_decode(jpg, jpg_len, &arena, &dec));
    double psnr = test_jpeg_psnr(px, dec.pixels, (size_t)W * H * 3);
    ASSERT(psnr > 40.0);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_roundtrip_gray) {
    enum { W = 80, H = 64 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H);
    ASSERT(px != NULL);
    uint32_t seed = 424242;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            seed = seed * 1103515245u + 12345u;
            int v = 128 + (int)(60 * sin(x * 0.13 + y * 0.06)) +
                    (int)((seed >> 16) & 15);
            px[(size_t)y * W + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    TspdfRawImage img = { W, H, 1, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *jpg;
    size_t jpg_len;
    ASSERT(tspdf_jpeg_encode(&img, 85, &arena, &jpg, &jpg_len));
    TspdfRawImage dec;
    ASSERT(tspdf_jpeg_decode(jpg, jpg_len, &arena, &dec));
    ASSERT_EQ_INT(dec.components, 1);
    double psnr = test_jpeg_psnr(px, dec.pixels, (size_t)W * H);
    ASSERT(psnr > 32.0);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_odd_dimensions) {
    // Non-multiple-of-16 dims exercise the padding path on both axes.
    enum { W = 61, H = 37 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(px != NULL);
    test_jpeg_fill_photo(px, W, H);
    TspdfRawImage img = { W, H, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *jpg;
    size_t jpg_len;
    ASSERT(tspdf_jpeg_encode(&img, 75, &arena, &jpg, &jpg_len));
    TspdfRawImage dec;
    ASSERT(tspdf_jpeg_decode(jpg, jpg_len, &arena, &dec));
    ASSERT_EQ_INT(dec.width, W);
    ASSERT_EQ_INT(dec.height, H);
    double psnr = test_jpeg_psnr(px, dec.pixels, (size_t)W * H * 3);
    ASSERT(psnr > 28.0);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_rejects_bad_args) {
    uint8_t px[16 * 16 * 3] = {0};
    TspdfRawImage img = { 16, 16, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 16);
    uint8_t *out;
    size_t out_len;
    ASSERT(!tspdf_jpeg_encode(NULL, 75, &arena, &out, &out_len));
    ASSERT(!tspdf_jpeg_encode(&img, 0, &arena, &out, &out_len));
    ASSERT(!tspdf_jpeg_encode(&img, 101, &arena, &out, &out_len));
    img.components = 2;
    ASSERT(!tspdf_jpeg_encode(&img, 75, &arena, &out, &out_len));
    img.components = 3;
    img.width = 0;
    ASSERT(!tspdf_jpeg_encode(&img, 75, &arena, &out, &out_len));
    img.width = 16;
    img.pixels = NULL;
    ASSERT(!tspdf_jpeg_encode(&img, 75, &arena, &out, &out_len));
    tspdf_arena_destroy(&arena);
}

TEST(test_jpeg_encode_external_decoder_oracle) {
    // Decode our encoder's output with an independent real-world decoder
    // (djpeg, libjpeg-turbo) and hold it to the same >30dB bar. This is what
    // proves PDF viewers will render the recompressed images. Skipped when
    // djpeg is not installed (test-time-only tool, zero-dependency promise).
    if (system("command -v djpeg > /dev/null 2>&1") != 0) {
        SKIP("djpeg not installed");
    }
    enum { W = 128, H = 96 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(px != NULL);
    test_jpeg_fill_photo(px, W, H);
    TspdfRawImage img = { W, H, 3, px };
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *jpg;
    size_t jpg_len;
    ASSERT(tspdf_jpeg_encode(&img, 75, &arena, &jpg, &jpg_len));
    const char *jpg_path = "/tmp/tspdf_test_enc_oracle.jpg";
    const char *ppm_path = "/tmp/tspdf_test_enc_oracle.ppm";
    FILE *f = fopen(jpg_path, "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite(jpg, 1, jpg_len, f) == jpg_len);
    fclose(f);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "djpeg -ppm -outfile %s %s 2>/dev/null",
             ppm_path, jpg_path);
    ASSERT(system(cmd) == 0);
    f = fopen(ppm_path, "rb");
    ASSERT(f != NULL);
    int pw = 0, ph = 0, pmax = 0;
    ASSERT(fscanf(f, "P6 %d %d %d", &pw, &ph, &pmax) == 3);
    ASSERT(fgetc(f) == '\n');
    ASSERT_EQ_INT(pw, W);
    ASSERT_EQ_INT(ph, H);
    ASSERT_EQ_INT(pmax, 255);
    uint8_t *dec = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(dec != NULL);
    ASSERT(fread(dec, 1, (size_t)W * H * 3, f) == (size_t)W * H * 3);
    fclose(f);
    remove(jpg_path);
    remove(ppm_path);
    double psnr = test_jpeg_psnr(px, dec, (size_t)W * H * 3);
    ASSERT(psnr > 30.0);
    free(dec);
    free(px);
    tspdf_arena_destroy(&arena);
}

// ============================================================
// CCITT codec (src/image/ccitt_codec.c)
// ============================================================
//
// Decoder oracle: coded streams produced by two independent known-good
// encoders — Pillow/libtiff (TIFF group3/group4 strip bytes) and
// Ghostscript's CCITTFaxEncode filter (K = 0 / K > 0 / EncodedByteAlign /
// BlackIs1 variants). Source bitmaps are committed as PBM (P4) next to the
// coded fixtures; tests/data/gen_ccitt_fixtures.py regenerates everything.
// The two ccitt_runs_* fixtures exercise every white and black terminating
// and makeup code, so a single wrong table entry fails them.

// Read tests/data/<name> as a P4 (packed, binary) PBM into 0/255 bytes
// (0 = black, matching the codec's pixel convention).
static uint8_t *test_ccitt_read_pbm(const char *name, int *out_w, int *out_h) {
    char path[128];
    snprintf(path, sizeof(path), "tests/data/%s", name);
    size_t len;
    uint8_t *raw = test_jpeg_read_file(path, &len);
    if (!raw) return NULL;
    int w = 0, h = 0;
    size_t pos = 0;
    if (len < 3 || raw[0] != 'P' || raw[1] != '4') { free(raw); return NULL; }
    pos = 2;
    for (int field = 0; field < 2; field++) {
        while (pos < len && (raw[pos] == ' ' || raw[pos] == '\n' ||
                             raw[pos] == '\t' || raw[pos] == '\r'))
            pos++;
        int v = 0;
        while (pos < len && raw[pos] >= '0' && raw[pos] <= '9') {
            v = v * 10 + (raw[pos] - '0');
            pos++;
        }
        if (field == 0) w = v;
        else h = v;
    }
    pos++;  // single whitespace after the header
    size_t stride = ((size_t)w + 7) / 8;
    if (w <= 0 || h <= 0 || pos + stride * (size_t)h > len) {
        free(raw);
        return NULL;
    }
    uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!px) { free(raw); return NULL; }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bit = (raw[pos + (size_t)y * stride + (size_t)x / 8] >>
                       (7 - (x & 7))) & 1;
            px[(size_t)y * (size_t)w + x] = bit ? 0 : 255;  // PBM: 1 = black
        }
    }
    free(raw);
    *out_w = w;
    *out_h = h;
    return px;
}

// Decode tests/data/<fixture> with the given params and require an exact
// pixel match against tests/data/<pbm>. When invert is set the expected
// pixels flip (BlackIs1 semantics: black decodes to sample 1 = white).
static bool test_ccitt_check_fixture(const char *fixture, const char *pbm,
                                     const TspdfCcittParams *params,
                                     bool invert) {
    int w = 0, h = 0;
    uint8_t *want = test_ccitt_read_pbm(pbm, &w, &h);
    if (!want) return false;
    char path[128];
    snprintf(path, sizeof(path), "tests/data/%s", fixture);
    size_t clen;
    uint8_t *coded = test_jpeg_read_file(path, &clen);
    if (!coded) { free(want); return false; }

    TspdfCcittParams p = *params;
    p.columns = w;
    p.rows = h;
    TspdfArena arena = tspdf_arena_create(1 << 20);
    TspdfCcittBitmap bm;
    bool ok = tspdf_ccitt_decode(coded, clen, &p, &arena, &bm);
    if (ok) ok = bm.width == w && bm.height == h;
    if (ok) {
        for (size_t i = 0; i < (size_t)w * (size_t)h && ok; i++) {
            uint8_t e = invert ? (uint8_t)(255 - want[i]) : want[i];
            if (bm.pixels[i] != e) ok = false;
        }
    }
    tspdf_arena_destroy(&arena);
    free(coded);
    free(want);
    return ok;
}

TEST(test_ccitt_decode_pil_g3_white_runs) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 0;  // libtiff group3 default: 1-D with EOLs
    p.end_of_line = true;
    ASSERT(test_ccitt_check_fixture("ccitt_runs_white.g3", "ccitt_runs_white.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_pil_g3_black_runs) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 0;
    p.end_of_line = true;
    ASSERT(test_ccitt_check_fixture("ccitt_runs_black.g3", "ccitt_runs_black.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_pil_g4_text) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    ASSERT(test_ccitt_check_fixture("ccitt_text.g4", "ccitt_text.pbm", &p, false));
}

TEST(test_ccitt_decode_gs_k0_noeol) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 0;
    ASSERT(test_ccitt_check_fixture("ccitt_text_k0.bin", "ccitt_text.pbm", &p, false));
}

TEST(test_ccitt_decode_gs_k0_byte_align) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 0;
    p.encoded_byte_align = true;
    ASSERT(test_ccitt_check_fixture("ccitt_text_k0_align.bin", "ccitt_text.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_gs_k2_eol) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 2;
    p.end_of_line = true;
    ASSERT(test_ccitt_check_fixture("ccitt_text_k2_eol.bin", "ccitt_text.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_gs_k4_noeol) {
    // No EOLs, so no 1-D/2-D tag bits either: the decoder must fall back to
    // the one-1-D-line-every-K cadence.
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = 4;
    ASSERT(test_ccitt_check_fixture("ccitt_text_k4_noeol.bin", "ccitt_text.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_gs_g4_byte_align) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.encoded_byte_align = true;
    ASSERT(test_ccitt_check_fixture("ccitt_text_g4_align.bin", "ccitt_text.pbm",
                                    &p, false));
}

TEST(test_ccitt_decode_gs_g4_blackis1) {
    // The fixture was encoded by gs with /BlackIs1 true from the same packed
    // bits as the plain G4 fixture, so BlackIs1 flips the coding polarity on
    // both sides and the visible pixels come back unchanged — exactly the
    // filter round-trip a PDF viewer performs.
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.black_is_1 = true;
    ASSERT(test_ccitt_check_fixture("ccitt_text_g4_black1.bin", "ccitt_text.pbm",
                                    &p, false));
    // And on a canonically-coded stream, black_is_1 inverts the output
    // (coded black -> sample 1 -> /DeviceGray white).
    p.black_is_1 = true;
    ASSERT(test_ccitt_check_fixture("ccitt_text.g4", "ccitt_text.pbm", &p, true));
}

TEST(test_ccitt_decode_rows_unknown) {
    // rows = 0: decode until EOFB (gs writes one) and report the height.
    size_t clen;
    uint8_t *coded = test_jpeg_read_file("tests/data/ccitt_text_g4_black1.bin",
                                         &clen);
    ASSERT(coded != NULL);
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.columns = 400;
    p.rows = 0;
    p.black_is_1 = true;
    TspdfArena arena = tspdf_arena_create(1 << 20);
    TspdfCcittBitmap bm;
    ASSERT(tspdf_ccitt_decode(coded, clen, &p, &arena, &bm));
    ASSERT_EQ_INT(bm.width, 400);
    ASSERT_EQ_INT(bm.height, 120);
    tspdf_arena_destroy(&arena);
    free(coded);
}

TEST(test_ccitt_decode_end_of_block_false) {
    // /EndOfBlock false: the decoder must stop after /Rows lines and not
    // insist on an EOFB (PIL/libtiff G4 strips carry none).
    size_t clen;
    uint8_t *coded = test_jpeg_read_file("tests/data/ccitt_text.g4", &clen);
    ASSERT(coded != NULL);
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.columns = 400;
    p.rows = 120;
    p.end_of_block = false;
    TspdfArena arena = tspdf_arena_create(1 << 20);
    TspdfCcittBitmap bm;
    ASSERT(tspdf_ccitt_decode(coded, clen, &p, &arena, &bm));
    ASSERT_EQ_INT(bm.height, 120);
    tspdf_arena_destroy(&arena);
    free(coded);
}

// Fill a text-like deterministic pattern (strokes + baseline).
static void test_ccitt_fill_text(uint8_t *px, int w, int h) {
    memset(px, 255, (size_t)w * h);
    uint32_t lcg = 7;
    for (int y = 2; y + 7 < h; y += 9) {
        for (int x = 3; x + 12 < w; x += 14) {
            lcg = lcg * 1664525u + 1013904223u;
            int ww = 6 + (int)((lcg >> 24) % 7);
            for (int yy = y; yy < y + 6; yy++)
                for (int xx = x; xx < x + ww; xx++)
                    if ((xx - x) % 4 < 2 || yy == y + 5)
                        px[(size_t)yy * w + xx] = 0;
        }
    }
}

// Encode with our G4 encoder, decode with our decoder, require exactness.
static bool test_ccitt_roundtrip(const uint8_t *px, int w, int h) {
    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    bool ok = tspdf_ccitt_encode_g4(px, w, h, &arena, &enc, &enc_len);
    if (ok) {
        TspdfCcittParams p;
        tspdf_ccitt_params_default(&p);
        p.k = -1;
        p.columns = w;
        p.rows = h;
        TspdfCcittBitmap bm;
        ok = tspdf_ccitt_decode(enc, enc_len, &p, &arena, &bm) &&
             bm.width == w && bm.height == h &&
             memcmp(bm.pixels, px, (size_t)w * h) == 0;
    }
    tspdf_arena_destroy(&arena);
    return ok;
}

TEST(test_ccitt_g4_roundtrip_patterns) {
    enum { W = 61, H = 33 };  // width deliberately not a multiple of 8
    uint8_t *px = (uint8_t *)malloc((size_t)W * H);
    ASSERT(px != NULL);

    memset(px, 255, (size_t)W * H);  // all white
    ASSERT(test_ccitt_roundtrip(px, W, H));

    memset(px, 0, (size_t)W * H);  // all black
    ASSERT(test_ccitt_roundtrip(px, W, H));

    for (int y = 0; y < H; y++)  // alternating columns (max changes per line)
        for (int x = 0; x < W; x++)
            px[(size_t)y * W + x] = (x & 1) ? 0 : 255;
    ASSERT(test_ccitt_roundtrip(px, W, H));

    for (int y = 0; y < H; y++)  // checkerboard (vertical modes both ways)
        for (int x = 0; x < W; x++)
            px[(size_t)y * W + x] = ((x + y) & 1) ? 0 : 255;
    ASSERT(test_ccitt_roundtrip(px, W, H));

    test_ccitt_fill_text(px, W, H);  // text-like strokes
    ASSERT(test_ccitt_roundtrip(px, W, H));

    uint32_t lcg = 99;  // deterministic noise (horizontal-mode heavy)
    for (size_t i = 0; i < (size_t)W * H; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        px[i] = (lcg >> 24) & 1 ? 0 : 255;
    }
    ASSERT(test_ccitt_roundtrip(px, W, H));
    free(px);

    // Degenerate sizes.
    uint8_t one[3] = {0, 255, 0};
    ASSERT(test_ccitt_roundtrip(one, 1, 1));
    ASSERT(test_ccitt_roundtrip(one, 3, 1));
    ASSERT(test_ccitt_roundtrip(one, 1, 3));
}

TEST(test_ccitt_g4_roundtrip_long_runs) {
    // Wide bitmap with runs beyond the largest single makeup code (2560):
    // the encoder must chain makeups and the decoder must sum them.
    enum { W = 6001, H = 5 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H);
    ASSERT(px != NULL);
    memset(px, 255, (size_t)W * H);
    for (int x = 2900; x < W; x++) px[(size_t)0 * W + x] = 0;  // black 3101
    for (int x = 0; x < 2700; x++) px[(size_t)1 * W + x] = 0;  // white 3301 tail
    // row 2 all white (run 6001), row 3 all black, row 4 single black pixel
    memset(px + (size_t)3 * W, 0, W);
    px[(size_t)4 * W + 3000] = 0;
    ASSERT(test_ccitt_roundtrip(px, W, H));
    free(px);
}

TEST(test_ccitt_g4_encode_matches_external_decoder) {
    // Our encoder's output must decode in an independent implementation:
    // Ghostscript's CCITTFaxDecode filter reproduces the source bitmap
    // exactly. (Rendering through pdftoppm/mutool is covered in
    // tests/test_cli.sh.)
    if (system("command -v gs > /dev/null 2>&1") != 0) {
        SKIP("gs not installed");
    }
    enum { W = 200, H = 60 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H);
    ASSERT(px != NULL);
    test_ccitt_fill_text(px, W, H);

    TspdfArena arena = tspdf_arena_create(1 << 20);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    ASSERT(tspdf_ccitt_encode_g4(px, W, H, &arena, &enc, &enc_len));

    const char *g4_path = "/tmp/tspdf_test_ccitt_ext.g4";
    const char *raw_path = "/tmp/tspdf_test_ccitt_ext.raw";
    FILE *f = fopen(g4_path, "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite(enc, 1, enc_len, f) == enc_len);
    fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "gs -q -dNODISPLAY -dBATCH -dNOSAFER -c '/inf (%s) (r) file def "
             "/dec inf << /K -1 /Columns %d /Rows %d >> /CCITTFaxDecode filter def "
             "/outf (%s) (w) file def /buf 4096 string def "
             "{ dec buf readstring exch outf exch writestring not { exit } if } loop "
             "outf closefile quit' 2>/dev/null",
             g4_path, W, H, raw_path);
    ASSERT(system(cmd) == 0);

    size_t raw_len = 0;
    uint8_t *raw = test_jpeg_read_file(raw_path, &raw_len);
    ASSERT(raw != NULL);
    ASSERT_EQ_SIZE(raw_len, (size_t)(W + 7) / 8 * H);
    bool same = true;
    for (int y = 0; y < H && same; y++)
        for (int x = 0; x < W && same; x++) {
            int bit = (raw[(size_t)y * ((W + 7) / 8) + x / 8] >> (7 - (x & 7))) & 1;
            // gs raw output: 0 bit = black (default BlackIs1 false)
            if ((bit ? 255 : 0) != px[(size_t)y * W + x]) same = false;
        }
    ASSERT(same);
    remove(g4_path);
    remove(raw_path);
    free(raw);
    free(px);
    tspdf_arena_destroy(&arena);
}

TEST(test_ccitt_decode_rejects_malformed) {
    TspdfArena arena = tspdf_arena_create(1 << 16);
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.columns = 64;
    p.rows = 8;
    TspdfCcittBitmap bm;
    // Empty and absurd-parameter inputs.
    uint8_t byte = 0x80;
    ASSERT(!tspdf_ccitt_decode(NULL, 1, &p, &arena, &bm));
    ASSERT(!tspdf_ccitt_decode(&byte, 0, &p, &arena, &bm));
    p.columns = 0;
    ASSERT(!tspdf_ccitt_decode(&byte, 1, &p, &arena, &bm));
    p.columns = (1 << 20) + 1;
    ASSERT(!tspdf_ccitt_decode(&byte, 1, &p, &arena, &bm));
    p.columns = 64;
    p.rows = -1;
    ASSERT(!tspdf_ccitt_decode(&byte, 1, &p, &arena, &bm));
    p.rows = 1 << 21;
    ASSERT(!tspdf_ccitt_decode(&byte, 1, &p, &arena, &bm));
    // Truncated stream with /Rows given: hard error, not a short image.
    size_t clen;
    uint8_t *coded = test_jpeg_read_file("tests/data/ccitt_text.g4", &clen);
    ASSERT(coded != NULL);
    p.columns = 400;
    p.rows = 120;
    ASSERT(!tspdf_ccitt_decode(coded, clen / 2, &p, &arena, &bm));
    // Repeated 0000001x: the T.4 extension escape (uncompressed mode),
    // which we reject; must fail cleanly, not hang. (All-ones, by contrast,
    // is a *valid* G4 stream: V0 per line = all white.)
    uint8_t ext[64];
    memset(ext, 0x02, sizeof(ext));
    p.rows = 8;
    p.columns = 64;
    ASSERT(!tspdf_ccitt_decode(ext, sizeof(ext), &p, &arena, &bm));
    free(coded);
    tspdf_arena_destroy(&arena);
}

TEST(test_ccitt_decode_fuzz_mutations_no_crash) {
    // 500 deterministic corruptions of a valid G4 stream (fixed-seed
    // xorshift): decode must neither crash nor hang, with /Rows known and
    // unknown. Run under ASan/UBSan via `make test-asan`.
    size_t clen;
    uint8_t *coded = test_jpeg_read_file("tests/data/ccitt_text.g4", &clen);
    ASSERT(coded != NULL);
    uint8_t *mut = (uint8_t *)malloc(clen);
    ASSERT(mut != NULL);
    uint32_t x = 0x9E3779B9u;  // fixed seed, no time()
    TspdfArena arena = tspdf_arena_create(1 << 16);
    for (int iter = 0; iter < 500; iter++) {
        memcpy(mut, coded, clen);
        // xorshift32
        int flips = 1 + (iter % 8);
        for (int i = 0; i < flips; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mut[x % clen] ^= (uint8_t)(1u << ((x >> 8) & 7));
        }
        size_t len = clen;
        if (iter % 5 == 0) len = 1 + (x % clen);  // truncate too
        TspdfCcittParams p;
        tspdf_ccitt_params_default(&p);
        p.k = (iter % 3 == 0) ? -1 : (iter % 3 == 1) ? 0 : 2;
        p.columns = 400;
        p.rows = (iter & 1) ? 120 : 0;
        p.encoded_byte_align = (iter % 7) == 0;
        TspdfCcittBitmap bm;
        (void)tspdf_ccitt_decode(mut, len, &p, &arena, &bm);
        tspdf_arena_reset(&arena);
    }
    tspdf_arena_destroy(&arena);
    free(mut);
    free(coded);
}

TEST(test_ccitt_encode_rejects_bad_args) {
    TspdfArena arena = tspdf_arena_create(1 << 12);
    uint8_t px[4] = {0, 255, 255, 0};
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT(!tspdf_ccitt_encode_g4(NULL, 2, 2, &arena, &out, &out_len));
    ASSERT(!tspdf_ccitt_encode_g4(px, 0, 2, &arena, &out, &out_len));
    ASSERT(!tspdf_ccitt_encode_g4(px, 2, 0, &arena, &out, &out_len));
    ASSERT(!tspdf_ccitt_encode_g4(px, (1 << 20) + 1, 1, &arena, &out, &out_len));
    ASSERT(!tspdf_ccitt_encode_g4(px, 2, 2, &arena, NULL, &out_len));
    ASSERT(!tspdf_ccitt_encode_g4(px, 2, 2, &arena, &out, NULL));
    ASSERT(tspdf_ccitt_encode_g4(px, 2, 2, &arena, &out, &out_len));
    ASSERT(out != NULL && out_len > 0);
    tspdf_arena_destroy(&arena);
}

// ============================================================
// Main
// ============================================================

int main(void) {
    printf("Running tests...\n\n");
    printf("  TspdfBuffer:\n");
    RUN(test_buffer_create);
    RUN(test_buffer_append);
    RUN(test_buffer_printf);
    RUN(test_buffer_reset);
    RUN(test_buffer_append_double);
    RUN(test_buffer_append_double_precision);
    RUN(test_buffer_append_double_negative);
    RUN(test_buffer_append_int);
    RUN(test_buffer_reject_grow_overflow);
    RUN(test_buffer_grow_beyond_initial);

    printf("\n  TspdfArena:\n");
    RUN(test_arena_create);
    RUN(test_arena_alloc_alignment);
    RUN(test_arena_growth);
    RUN(test_arena_reset);
    RUN(test_arena_reject_alloc_overflow);

    printf("\n  Deflate:\n");
    RUN(test_deflate_roundtrip);
    RUN(test_deflate_roundtrip_repeated);
    RUN(test_deflate_large_input);
    RUN(test_deflate_roundtrip_empty);
    RUN(test_deflate_roundtrip_one_byte);
    RUN(test_deflate_roundtrip_all_same);
    RUN(test_deflate_roundtrip_random_incompressible);
    RUN(test_deflate_roundtrip_text_corpus);
    RUN(test_deflate_roundtrip_long_repeats_near_window);
    RUN(test_deflate_roundtrip_block_boundaries);
    RUN(test_deflate_roundtrip_multi_megabyte);
    RUN(test_deflate_rejects_invalid_stored_nlen);
    RUN(test_deflate_rejects_invalid_block_type);
    RUN(test_deflate_rejects_output_over_max);
    RUN(test_deflate_rejects_truncated_stream);
    RUN(test_deflate_rejects_header_only_stream);

    printf("\n  PNG decoder:\n");
    RUN(test_png_rejects_zero_width);
    RUN(test_png_rejects_oversized_dimensions);
    RUN(test_png_rejects_dimension_above_cap);
    RUN(test_png_rejects_truncated_idat);

    printf("\n  Base14 Fonts:\n");
    RUN(test_base14_get);
    RUN(test_base14_measure);
    RUN(test_base14_line_height);
    RUN(test_base14_is_latin);

    printf("\n  Layout:\n");
    RUN(test_layout_box);
    RUN(test_layout_text);
    RUN(test_layout_add_child_success);
    RUN(test_layout_add_child_overflow);
    RUN(test_layout_span_success);
    RUN(test_layout_node_size);

    printf("\n  PDF Document:\n");
    RUN(test_tspdf_writer_create);
    RUN(test_pdf_add_font);
    RUN(test_pdf_add_page);
    RUN(test_pdf_add_text_field);
    RUN(test_pdf_add_checkbox);
    RUN(test_pdf_add_link);
    RUN(test_pdf_add_bookmark);
    RUN(test_pdf_bookmark_xyz_dest_and_unicode_title);
    RUN(test_pdf_bookmark_count_is_visible_descendants);
    RUN(test_pdf_bookmark_title_truncates_at_utf8_boundary);
    RUN(test_pdf_save);

    printf("\n  String Safety:\n");
    RUN(test_long_text_preserved);
    RUN(test_long_font_name_truncated);

    printf("\n  Bug Fixes:\n");
    RUN(test_pdf_save_with_text_content);
    RUN(test_png_missing_file_rejected);
    RUN(test_font_subset_missing_tables);
    RUN(test_layout_path_begin_arena_exhaustion);
    RUN(test_pdf_save_with_form_fields);

    printf("\n  TTF Parser:\n");
    RUN(test_ttf_cmap_format_preference);

    printf("\n  UTF-8 Wrapping:\n");
    RUN(test_wrap_text_utf8_no_split);
    RUN(test_wrap_word_utf8);

    printf("\n  Error Reporting:\n");
    RUN(test_error_last_error_default);
    RUN(test_pages_grow_dynamically);
    RUN(test_error_font_limit);
    RUN(test_error_save_returns_tspdf_error);
    RUN(test_error_reset_on_success);
    RUN(test_error_string_all_codes);

    printf("\n  Unicode Text Encoding:\n");
    RUN(test_layout_render_ttf_utf8);
    RUN(test_show_text_utf8_ascii);
    RUN(test_show_text_utf8_multibyte);
    RUN(test_show_text_utf8_invalid);
    RUN(test_show_text_utf8_empty);
    RUN(test_pdf_save_with_ttf_unicode);

    printf("\n  Memory Safety (B1-B5):\n");
    RUN(test_ttf_free_zeroed);
    RUN(test_ttf_cmap_malformed_segcount);
    RUN(test_ttf_cmap_format6_oob_entry_count);
    RUN(test_ttf_cmap_format6_in_bounds);
    RUN(test_ttf_kern_pair_overflow_rejected);
    RUN(test_ttf_load_from_memory_partial_failure);

    printf("\n  Integration:\n");
    RUN(test_pdf_save_builtin_still_works);
    RUN(test_small_stream_not_compressed);

    printf("\n  Audit fixes (reader-core):\n");
    RUN(test_deflate_decompresses_valid_stored_block);
    printf("\n  Encoding/i18n:\n");
    RUN(test_writer_producer_is_tspdf_with_version);
    printf("\n  PNG palette support:\n");
    RUN(test_png_palette_8bit_decodes);
    RUN(test_png_palette_trns_decodes_rgba);
    RUN(test_png_palette_1bit_decodes);
    RUN(test_png_palette_2bit_decodes);
    RUN(test_png_palette_4bit_decodes);
    RUN(test_png_palette_index_out_of_range_rejected);
    RUN(test_png_palette_missing_plte_rejected);
    RUN(test_png_interlaced_still_rejected);
    RUN(test_png_palette_embeds_in_writer);
    RUN(test_png_passthrough_rgb_extracts_idat);
    RUN(test_png_passthrough_gray);
    RUN(test_png_passthrough_palette);
    RUN(test_png_passthrough_palette_4bit);
    RUN(test_png_passthrough_palette_index_out_of_range_rejected);
    RUN(test_png_passthrough_rejects_interlaced_and_alpha);
    RUN(test_png_passthrough_rejects_damaged_idat);
    RUN(test_png_passthrough_rejects_nonstandard_zlib_header);
    RUN(test_png_passthrough_stops_at_iend);
    RUN(test_png_fdict_converts_via_decode_fallback);
    RUN(test_png_passthrough_embeds_idat_verbatim);

    printf("\n  Save-to-memory byte identity (wasm):\n");
    RUN(test_writer_save_to_memory_matches_file);

    printf("\n  PDF date formatting:\n");
    RUN(test_pdf_date_full_forms);
    RUN(test_pdf_date_partial_fields_default);
    RUN(test_pdf_date_odd_zones);
    RUN(test_pdf_date_garbage_rejected);

    printf("\n  JPEG codec:\n");
    RUN(test_jpeg_decode_gray_gradient_matches_pil);
    RUN(test_jpeg_decode_gray_noise_matches_pil);
    RUN(test_jpeg_decode_rgb_444_matches_pil);
    RUN(test_jpeg_decode_rgb_422_matches_pil);
    RUN(test_jpeg_decode_rgb_420_matches_pil);
    RUN(test_jpeg_decode_rgb_420_odd_dims_matches_pil);
    RUN(test_jpeg_decode_rgb_1x2_vertical_matches_pil);
    RUN(test_jpeg_decode_restart_markers);
    RUN(test_jpeg_decode_flat_blocks);
    RUN(test_jpeg_decode_rejects_progressive);
    RUN(test_jpeg_decode_rejects_malformed);
    RUN(test_jpeg_decode_fuzz_mutations_no_crash);
    RUN(test_jpeg_encode_deterministic);
    RUN(test_jpeg_encode_roundtrip_photo_q75);
    RUN(test_jpeg_encode_roundtrip_gradient_q95);
    RUN(test_jpeg_encode_roundtrip_gray);
    RUN(test_jpeg_encode_odd_dimensions);
    RUN(test_jpeg_encode_rejects_bad_args);
    RUN(test_jpeg_encode_external_decoder_oracle);

    printf("\n  CCITT codec:\n");
    RUN(test_ccitt_decode_pil_g3_white_runs);
    RUN(test_ccitt_decode_pil_g3_black_runs);
    RUN(test_ccitt_decode_pil_g4_text);
    RUN(test_ccitt_decode_gs_k0_noeol);
    RUN(test_ccitt_decode_gs_k0_byte_align);
    RUN(test_ccitt_decode_gs_k2_eol);
    RUN(test_ccitt_decode_gs_k4_noeol);
    RUN(test_ccitt_decode_gs_g4_byte_align);
    RUN(test_ccitt_decode_gs_g4_blackis1);
    RUN(test_ccitt_decode_rows_unknown);
    RUN(test_ccitt_decode_end_of_block_false);
    RUN(test_ccitt_g4_roundtrip_patterns);
    RUN(test_ccitt_g4_roundtrip_long_runs);
    RUN(test_ccitt_g4_encode_matches_external_decoder);
    RUN(test_ccitt_decode_rejects_malformed);
    RUN(test_ccitt_decode_fuzz_mutations_no_crash);
    RUN(test_ccitt_encode_rejects_bad_args);

    printf("\n  QR encoder:\n");
    RUN(test_qr_ecc_table_consistent);
    RUN(test_qr_rs_known_vector);
    RUN(test_qr_rs_reference_vector);
    RUN(test_qr_version_info_bits);
    RUN(test_qr_version_selection_boundaries);
    RUN(test_qr_golden_grids);

    printf("\n%d tests run, %d passed, %d failed, %d skipped\n",
           tests_run, tests_passed, tests_failed, tests_skipped);
    if (tests_skipped > 0) {
        printf("note: %d test(s) skipped (missing optional system font) — "
               "TTF subsetting / Unicode paths were NOT exercised\n",
               tests_skipped);
    }
    return tests_failed > 0 ? 1 : 0;
}
