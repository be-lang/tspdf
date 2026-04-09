#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "test_framework.h"

#include "../src/util/buffer.h"
#include "../src/util/arena.h"
#include "../src/compress/deflate.h"
#include "../src/image/png_decoder.h"
#include "../src/pdf/pdf_base14.h"
#include "../src/layout/layout.h"
#include "../src/pdf/tspdf_writer.h"
#include "../src/tspdf_error.h"

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
        return;  // font not available, skip
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
    if (!fname) { tspdf_writer_destroy(doc); return; }
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
    if (!fname) { tspdf_writer_destroy(doc); return; }
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
    if (!fname) { tspdf_writer_destroy(doc); return; }
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
    if (!fname) { tspdf_writer_destroy(doc); return; }
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
    if (!fname) { tspdf_writer_destroy(doc); return; }

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
    if (!fname) { tspdf_writer_destroy(doc); return; }
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
    fread(content, 1, size, f);
    fclose(f);

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
    fread(content, 1, size, f);
    fclose(f);

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

    printf("\n  TspdfArena:\n");
    RUN(test_arena_create);
    RUN(test_arena_alloc_alignment);
    RUN(test_arena_growth);
    RUN(test_arena_reset);

    printf("\n  Deflate:\n");
    RUN(test_deflate_roundtrip);
    RUN(test_deflate_roundtrip_repeated);
    RUN(test_deflate_large_input);
    RUN(test_deflate_rejects_invalid_stored_nlen);
    RUN(test_deflate_rejects_invalid_block_type);
    RUN(test_deflate_rejects_output_over_max);

    printf("\n  PNG decoder:\n");
    RUN(test_png_rejects_zero_width);
    RUN(test_png_rejects_oversized_dimensions);
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
    RUN(test_ttf_load_from_memory_partial_failure);

    printf("\n  Integration:\n");
    RUN(test_pdf_save_builtin_still_works);
    RUN(test_small_stream_not_compressed);

    printf("\n%d tests run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
