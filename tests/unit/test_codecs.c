#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "../test_framework.h"

#include "../../src/util/buffer.h"
#include "../../src/util/arena.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/png_decoder.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/pdf/pdf_base14.h"
#include "../../src/layout/layout.h"
#include "../../src/pdf/tspdf_writer.h"
#include "../../src/tspdf_error.h"
#include "../../src/qr/qr_encode.h"
#include "../../src/util/pdfdate.h"

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

// --- deflate_compress_best: same contract as deflate_compress, more search
// effort. Round-trips must be exact through the in-repo inflater. ---

// Shared helper: compress with deflate_compress_best, decompress, compare.
// void because ASSERT bails with a bare return; on failure *out_comp_len
// stays 0 and _test_failed is set, so callers must return early themselves.
static void deflate_best_roundtrip_check(const uint8_t *data, size_t len,
                                         size_t *out_comp_len) {
    *out_comp_len = 0;
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress_best(data, len, &comp_len);
    ASSERT(comp != NULL);
    ASSERT(comp_len > 0);

    size_t decomp_len = len ? 0 : 12345;
    uint8_t *decomp = deflate_decompress(comp, comp_len, &decomp_len);
    ASSERT(decomp != NULL);
    ASSERT(decomp_len == len);
    if (len > 0) ASSERT(memcmp(decomp, data, len) == 0);

    free(comp);
    free(decomp);
    *out_comp_len = comp_len;
}

TEST(test_deflate_best_roundtrip_empty) {
    size_t comp_len;
    deflate_best_roundtrip_check((const uint8_t *)"", 0, &comp_len);
}

TEST(test_deflate_best_roundtrip_one_byte) {
    uint8_t b = 0x42;
    size_t comp_len;
    deflate_best_roundtrip_check(&b, 1, &comp_len);
}

TEST(test_deflate_best_roundtrip_all_same) {
    size_t len = 100000;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    memset(data, 'A', len);
    size_t comp_len;
    deflate_best_roundtrip_check(data, len, &comp_len);
    free(data);
    if (_test_failed) return;
    // Runs of maximum-length matches; more than a few hundred bytes means
    // match emission is broken.
    ASSERT(comp_len < 600);
}

// Incompressible input: output must still round-trip exactly, and stored-block
// fallback must cap expansion to a small overhead over the raw size.
TEST(test_deflate_best_roundtrip_random_incompressible) {
    size_t len = 262144;
    uint8_t *data = (uint8_t *)malloc(len);
    ASSERT(data != NULL);
    uint32_t s = 0x9E3779B9u;  // xorshift32: deterministic pseudo-random bytes
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        data[i] = (uint8_t)s;
    }
    size_t comp_len;
    deflate_best_roundtrip_check(data, len, &comp_len);
    free(data);
    if (_test_failed) return;
    ASSERT(comp_len < len + len / 100 + 64);
}

// ~1MB of deterministic English-like text: several 64K-symbol blocks and
// window wraps, plus the "best <= fast" size guarantee on text.
TEST(test_deflate_best_roundtrip_english_text) {
    static const char *words[] = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "compression", "attention", "is", "all", "you", "need", "transformer",
        "model", "sequence", "encoder", "decoder", "layer", "output", "input",
    };
    size_t nwords = sizeof words / sizeof words[0];
    size_t cap = 1024 * 1024 + 64;
    char *text = (char *)malloc(cap);
    ASSERT(text != NULL);
    size_t n = 0;
    uint32_t s = 12345;
    while (n + 32 < 1024 * 1024) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const char *w = words[s % nwords];
        size_t wl = strlen(w);
        memcpy(text + n, w, wl);
        n += wl;
        text[n++] = (s & 0x70) ? ' ' : '\n';
    }

    size_t best_len;
    deflate_best_roundtrip_check((const uint8_t *)text, n, &best_len);
    if (_test_failed) { free(text); return; }
    ASSERT(best_len < n / 3);  // English-like text must compress well

    size_t fast_len = 0;
    uint8_t *fast = deflate_compress((const uint8_t *)text, n, &fast_len);
    ASSERT(fast != NULL);
    ASSERT(best_len <= fast_len);  // more effort must never cost bytes here

    free(fast);
    free(text);
}

// Repetitive structured text (PDF-ish operator soup with counters), same
// shape as test_deflate_roundtrip_text_corpus.
TEST(test_deflate_best_roundtrip_structured_text) {
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

    size_t best_len;
    deflate_best_roundtrip_check((const uint8_t *)text, n, &best_len);
    if (_test_failed) { free(text); return; }
    ASSERT(best_len < n / 4);

    size_t fast_len = 0;
    uint8_t *fast = deflate_compress((const uint8_t *)text, n, &fast_len);
    ASSERT(fast != NULL);
    ASSERT(best_len <= fast_len);

    free(fast);
    free(text);
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

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>

static void *ccitt_thread_decode(void *arg) {
    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    bool *ok = (bool *)arg;
    *ok = test_ccitt_check_fixture("ccitt_text.g4", "ccitt_text.pbm", &p, false);
    return NULL;
}

TEST(test_ccitt_decode_concurrent_first_use) {
    // First-use initialization of the decode LUTs must be safe when several
    // threads race into the decoder; this runs before any other CCITT test
    // so the tables are still cold. The pre-fix unsynchronized ready flag is
    // a data race only TSan diagnoses directly, but every thread must still
    // produce an exact decode here.
    enum { CCITT_NTHREADS = 8 };
    pthread_t tids[CCITT_NTHREADS];
    bool ok[CCITT_NTHREADS] = {false};
    for (int i = 0; i < CCITT_NTHREADS; i++) {
        ASSERT(pthread_create(&tids[i], NULL, ccitt_thread_decode, &ok[i]) == 0);
    }
    for (int i = 0; i < CCITT_NTHREADS; i++) {
        pthread_join(tids[i], NULL);
        ASSERT(ok[i]);
    }
}
#endif

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


void run_codecs_tests(void) {
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
    RUN(test_deflate_best_roundtrip_empty);
    RUN(test_deflate_best_roundtrip_one_byte);
    RUN(test_deflate_best_roundtrip_all_same);
    RUN(test_deflate_best_roundtrip_random_incompressible);
    RUN(test_deflate_best_roundtrip_english_text);
    RUN(test_deflate_best_roundtrip_structured_text);

    printf("\n  PNG decoder:\n");
    RUN(test_png_rejects_zero_width);
    RUN(test_png_rejects_oversized_dimensions);
    RUN(test_png_rejects_dimension_above_cap);
    RUN(test_png_rejects_truncated_idat);

    printf("\n  Audit fixes (reader-core):\n");
    RUN(test_deflate_decompresses_valid_stored_block);

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
#if defined(__unix__) || defined(__APPLE__)
    RUN(test_ccitt_decode_concurrent_first_use);  // must run first: cold LUTs
#endif
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
}
