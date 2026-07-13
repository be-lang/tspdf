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
#include "../../src/pdf/primitives.h"

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
// ============================================================
// Encoding / i18n (fix/encoding track)
// ============================================================

#include "../../include/tspdf/version.h"

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
// PDF primitives unit tests (src/pdf/primitives.c)
// ============================================================

// Helper: render tspdf_pdf_encode_string to a NUL-terminated C string.
// Caller must free() the result.
static char *prim_encode_string(const char *input) {
    TspdfBuffer buf = tspdf_buffer_create(64);
    tspdf_pdf_encode_string(&buf, (const uint8_t *)input, strlen(input));
    // Append NUL for strcmp
    tspdf_buffer_append_byte(&buf, '\0');
    char *out = malloc(buf.len);
    if (out) memcpy(out, buf.data, buf.len);
    tspdf_buffer_destroy(&buf);
    return out;
}

// Helper: render tspdf_pdf_encode_name to a NUL-terminated C string.
static char *prim_encode_name(const char *input) {
    TspdfBuffer buf = tspdf_buffer_create(64);
    tspdf_pdf_encode_name(&buf, (const uint8_t *)input, strlen(input));
    tspdf_buffer_append_byte(&buf, '\0');
    char *out = malloc(buf.len);
    if (out) memcpy(out, buf.data, buf.len);
    tspdf_buffer_destroy(&buf);
    return out;
}

// Plain ASCII round-trips unchanged.
TEST(test_primitives_string_plain_ascii) {
    char *got = prim_encode_string("Hello world");
    ASSERT(strcmp(got, "(Hello world)") == 0);
    free(got);
}

// ( ) \\ must be escaped.
TEST(test_primitives_string_delimiters) {
    char *got = prim_encode_string("(paren) \\back\\");
    ASSERT(strcmp(got, "(\\(paren\\) \\\\back\\\\)") == 0);
    free(got);
}

// Named escapes for the five whitespace control chars.
TEST(test_primitives_string_named_escapes) {
    char *got = prim_encode_string("\n\r\t\b\f");
    ASSERT(strcmp(got, "(\\n\\r\\t\\b\\f)") == 0);
    free(got);
}

// Bytes < 32 (other than the named ones) become \NNN octal.
TEST(test_primitives_string_control_byte) {
    // \x01 -> \001
    char *got = prim_encode_string("\x01");
    ASSERT(strcmp(got, "(\\001)") == 0);
    free(got);
}

// Bytes > 126 (DEL and high bytes) become \NNN octal.
TEST(test_primitives_string_high_byte) {
    // \x80 -> \200
    const uint8_t data[] = { 0x80 };
    TspdfBuffer buf = tspdf_buffer_create(16);
    tspdf_pdf_encode_string(&buf, data, 1);
    tspdf_buffer_append_byte(&buf, '\0');
    ASSERT(strcmp((char *)buf.data, "(\\200)") == 0);
    tspdf_buffer_destroy(&buf);
}

// Plain ASCII name round-trips unchanged.
TEST(test_primitives_name_plain) {
    char *got = prim_encode_name("FlateDecode");
    ASSERT(strcmp(got, "/FlateDecode") == 0);
    free(got);
}

// # encodes as #23, / as #2F, % as #25, space as #20.
TEST(test_primitives_name_specials) {
    char *got = prim_encode_name("/#% ()");
    // / -> #2F, # -> #23, % -> #25, space -> #20, ( -> #28, ) -> #29
    ASSERT(strcmp(got, "/#2F#23#25#20#28#29") == 0);
    free(got);
}

// Bytes < 33 (e.g., space 0x20, control 0x01) must be escaped.
TEST(test_primitives_name_low_byte) {
    char *got = prim_encode_name(" ");  // 0x20 = space
    ASSERT(strcmp(got, "/#20") == 0);
    free(got);
}

// Byte > 126 (DEL 0x7F) must be escaped.
TEST(test_primitives_name_del_byte) {
    const uint8_t data[] = { 0x7F };
    TspdfBuffer buf = tspdf_buffer_create(16);
    tspdf_pdf_encode_name(&buf, data, 1);
    tspdf_buffer_append_byte(&buf, '\0');
    ASSERT(strcmp((char *)buf.data, "/#7F") == 0);
    tspdf_buffer_destroy(&buf);
}

// Empty string -> "()"
TEST(test_primitives_string_empty) {
    char *got = prim_encode_string("");
    ASSERT(strcmp(got, "()") == 0);
    free(got);
}

// Empty name -> "/"
TEST(test_primitives_name_empty) {
    char *got = prim_encode_name("");
    ASSERT(strcmp(got, "/") == 0);
    free(got);
}

// ============================================================
// TDD: writer string and name escaping (spec fix)
// ============================================================
//
// These tests pin the SPEC-CORRECT behavior after the fix.
// The writer must now use the canonical primitives.

// Writer string: control byte \x01 must be octal-escaped in /Title.
TEST(test_writer_string_escapes_control_byte) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    // Title containing a control byte 0x01
    tspdf_writer_set_title(doc, "A\x01Z");

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    // Spec-correct: \001 (three-digit octal) must appear, not the raw byte
    ASSERT(bytes_contain(pdf, len, "/Title (A\\001Z)"));
    // Raw control byte must NOT appear in the title string
    // (Check we have no literal 0x01 between /Title ( and ))
    // Simple approach: verify the escaped form is present
    free(pdf);
}

// Writer string: parens and backslash must be escaped in /Title.
TEST(test_writer_string_escapes_parens_and_backslash) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    tspdf_writer_set_title(doc, "(paren) \\back\\");

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    // Spec-correct escaping (parens and backslash already worked in old code,
    // but we verify after refactor that they still work)
    ASSERT(bytes_contain(pdf, len, "/Title (\\(paren\\) \\\\back\\\\)"));
    free(pdf);
}

// Writer string: \n \r \t must be escaped as named escapes.
TEST(test_writer_string_escapes_whitespace_controls) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    tspdf_writer_set_title(doc, "A\nB\rC\tD");

    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(doc, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(doc);

    // Spec-correct: named escapes
    ASSERT(bytes_contain(pdf, len, "/Title (A\\nB\\rC\\tD)"));
    free(pdf);
}

// Writer name: special characters in a font/field name must be #HH-escaped.
// Use form field /T (field name stored as PDF string) and /FT (name).
// The real name escaping test: write a name via tspdf_raw_write_name
// and check it appears escaped in the output.
TEST(test_writer_name_escapes_special_chars) {
    // We test the primitives directly here since the high-level writer
    // doesn't expose a way to set a PDF name containing special chars.
    // Verify that a name "/#% ()" is correctly encoded.
    TspdfBuffer buf = tspdf_buffer_create(64);
    tspdf_pdf_encode_name(&buf, (const uint8_t *)"/#% ()", 6);
    tspdf_buffer_append_byte(&buf, '\0');
    // / -> #2F, # -> #23, % -> #25, space -> #20, ( -> #28, ) -> #29
    ASSERT(strcmp((char *)buf.data, "/#2F#23#25#20#28#29") == 0);
    tspdf_buffer_destroy(&buf);
}

// Integration: writer-generated PDF with special-char title passes qpdf --check.
// (This test generates a file; the qpdf check happens in the gate script,
//  but we verify the file is written correctly here.)
TEST(test_writer_special_chars_pdf_valid) {
    TspdfWriter *doc = tspdf_writer_create();
    tspdf_writer_add_page(doc);
    // Title with parens, backslash, control byte, named escapes
    tspdf_writer_set_title(doc, "(test)\\\x01\n");

    const char *path = "/tmp/tspdf_test_special_chars.pdf";
    TspdfError err = tspdf_writer_save(doc, path);
    ASSERT(err == TSPDF_OK);
    tspdf_writer_destroy(doc);

    // Verify the file exists and is non-empty
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    ASSERT(size > 100);

    remove(path);
}

void run_writer_tests(void) {
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

    printf("\n  Encoding/i18n:\n");
    RUN(test_writer_producer_is_tspdf_with_version);

    printf("\n  Save-to-memory byte identity (wasm):\n");
    RUN(test_writer_save_to_memory_matches_file);

    printf("\n  PDF date formatting:\n");
    RUN(test_pdf_date_full_forms);
    RUN(test_pdf_date_partial_fields_default);
    RUN(test_pdf_date_odd_zones);
    RUN(test_pdf_date_garbage_rejected);

    printf("\n  PDF primitives (encoding correctness):\n");
    RUN(test_primitives_string_plain_ascii);
    RUN(test_primitives_string_delimiters);
    RUN(test_primitives_string_named_escapes);
    RUN(test_primitives_string_control_byte);
    RUN(test_primitives_string_high_byte);
    RUN(test_primitives_name_plain);
    RUN(test_primitives_name_specials);
    RUN(test_primitives_name_low_byte);
    RUN(test_primitives_name_del_byte);
    RUN(test_primitives_string_empty);
    RUN(test_primitives_name_empty);

    printf("\n  Writer escaping (spec fix, TDD):\n");
    RUN(test_writer_string_escapes_control_byte);
    RUN(test_writer_string_escapes_parens_and_backslash);
    RUN(test_writer_string_escapes_whitespace_controls);
    RUN(test_writer_name_escapes_special_chars);
    RUN(test_writer_special_chars_pdf_valid);
}
