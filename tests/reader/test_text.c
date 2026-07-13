#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "../test_framework.h"
#include "../../src/reader/tspr.h"
#include "../../src/reader/tspr_overlay.h"
#include "../../src/reader/tspr_internal.h"
#include "../../src/pdf/pdf_stream.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/util/arena.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/font/ttf_parser.h"
#include "../../src/font/font_fallback.h"
#include "../../src/crypto/md5.h"
#include "../../include/tspdf/version.h"

#include "../../src/reader/tspr_text.h"

static bool appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= cap - *pos) {
        return false;
    }

    *pos += (size_t)written;
    return true;
}

static char *make_text_pdf_full(const char *resources, const char *font_body,
                                const char *aux_dict, const char *aux_data,
                                const char *content, size_t *out_len) {
    size_t cap = 8192 + strlen(content) + (aux_data ? strlen(aux_data) : 0);
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources %s /Contents 6 0 R >>\nendobj\n", resources)) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n%s\nendobj\n",
                 font_body ? font_body : "null")) goto fail;
    size_t obj5 = pos;
    if (aux_data) {
        if (!appendf(pdf, cap, &pos,
                     "5 0 obj\n<< %s /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                     aux_dict ? aux_dict : "", strlen(aux_data), aux_data)) goto fail;
    } else {
        if (!appendf(pdf, cap, &pos, "5 0 obj\n%s\nendobj\n",
                     aux_dict ? aux_dict : "null")) goto fail;
    }
    size_t obj6 = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

#define TEXT_HELV_FONT \
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"

#define TEXT_HELV_RES "<< /Font << /F1 4 0 R >> >>"

static char *make_text_pdf(const char *font_body, const char *content, size_t *out_len) {
    return make_text_pdf_full(TEXT_HELV_RES, font_body, NULL, NULL, content, out_len);
}

static const char *make_no_space_font(char *buf, size_t cap) {
    size_t pos = 0;
    appendf(buf, cap, &pos,
            "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+FakeItalic "
            "/Encoding /WinAnsiEncoding /FirstChar 40 /LastChar 122 /Widths [");
    for (int i = 40; i <= 122; i++) appendf(buf, cap, &pos, "500 ");
    appendf(buf, cap, &pos, "] >>");
    return buf;
}

#define TEXT_TWO_FONT_RES "<< /Font << /F1 4 0 R /F2 5 0 R >> >>"

static char *make_text_pdf_contents_array(size_t *out_len) {
    const char *s5 = "BT /F1 12 Tf 72 700 Td (One)";
    const char *s6 = "Tj ET";
    const char *s7 = "BT /F1 12 Tf 72 680 Td (Two) Tj ET";
    size_t cap = 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources " TEXT_HELV_RES " /Contents [5 0 R 6 0 R 7 0 R] >>\n"
                 "endobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n" TEXT_HELV_FONT "\nendobj\n")) goto fail;
    size_t obj5 = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s5), s5)) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s6), s6)) goto fail;
    size_t obj7 = pos;
    if (!appendf(pdf, cap, &pos,
                 "7 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s7), s7)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 8\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 8 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static bool text_line(const char *text, size_t n, char *buf, size_t buf_size) {
    const char *p = text;
    for (size_t i = 0; i < n; i++) {
        p = strchr(p, '\n');
        if (!p) return false;
        p++;
    }
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len + 1 > buf_size) return false;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

TEST(test_text_simple_lines_writer_fixture) {
    // Writer-generated ground truth: two lines with WinAnsi umlauts/dash.
    TspdfWriter *w = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 12.0);
    tspdf_stream_text_position(page, 72, 720);
    tspdf_stream_show_text(page, "Hello World");
    tspdf_stream_text_position(page, 0, -14);
    tspdf_stream_show_text(page, "Gr\xfc\xdf" "e \x96 Umlaute");  // cp1252 bytes
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello World\nGr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 Umlaute\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tj_kerning_spacing) {
    // Kerning tweaks (-30) must not create spaces; word gaps (-300) must.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td [(Hel) -30 (lo) -300 (world)] TJ ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello world\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_multiline_td_tstar) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 14 TL 72 700 Td (Line one) Tj T* (Line two) Tj "
        "0 -20 Td (Line three) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Line one\nLine two\nLine three\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_xgap_emits_space) {
    // Two runs on one baseline with a large x-gap: joined by a space,
    // not a newline.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Left) Tj ET "
        "BT /F1 12 Tf 200 700 Td (Right) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Left Right\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_superscript_joins_line) {
    // A superscript run (smaller font, baseline raised ~0.35 em of the base
    // font) must stay on the current line, and text returning to the base
    // baseline right after must too: "1.0 x 10" + sup "20" + "rest" is one
    // line, not three. Base run ends at x = 72 + 3.580 * 12 = 114.96, so the
    // superscript at x=115 attaches without a space; "rest" at x=130 sits a
    // real gap after the superscript's end (~123.9) and gets one space.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (1.0 x 10) Tj ET "
        "BT /F1 8 Tf 115 704.2 Td (20) Tj ET "
        "BT /F1 12 Tf 130 700 Td (rest) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "1.0 x 1020 rest\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_close_lines_stay_distinct) {
    // Genuinely distinct close-set lines must not be glued by the superscript
    // tolerance: 12 pt lines 6.6 pt apart (0.55 em, tighter than any real
    // leading) and an 8 pt footnote line 8 pt below (0.67 of the larger em)
    // each stay on their own line.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (First) Tj ET "
        "BT /F1 12 Tf 72 693.4 Td (Second) Tj ET "
        "BT /F1 8 Tf 72 685.4 Td (Third) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "First\nSecond\nThird\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tj_kern_space_missing_space_glyph) {
    // TeX-generated PDFs encode inter-word spaces as -250-ish TJ kerns and
    // often subset the space glyph away (FirstChar > 32). A 0.25 em kern
    // must still produce a space; a small -30 kerning tweak must not.
    char font[4096];
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT,
        make_no_space_font(font, sizeof(font)), NULL,
        "BT /F2 10 Tf 72 700 Td [(arXiv)-250(preprint)-30(s)] TJ ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "arXiv preprints\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_font_switch_gap_space) {
    // Font change at a word boundary (roman -> italic math, TeX style): the
    // x-gap between runs is a normal ~0.25 em word space, and the italic
    // font has no space-width data. A space must be inserted. The roman
    // comma that follows the italic run with no gap must stay glued.
    // Helvetica "values of" at 10pt is 40.02 wide (ends at 112.02); the /F2
    // run starts at 114.5 => 2.48 gap (~0.25 em). "d" is 500/1000 wide, so
    // it ends at 119.5 where the comma resumes with zero gap.
    char font[4096];
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT,
        make_no_space_font(font, sizeof(font)), NULL,
        "BT /F1 10 Tf 1 0 0 1 72 700 Tm (values of) Tj "
        "/F2 10 Tf 1 0 0 1 114.5 700 Tm (d) Tj "
        "/F1 10 Tf 1 0 0 1 119.5 700 Tm (,) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "values of d,\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_font_switch_bogus_space_width) {
    // TeX math fonts (cmmi) have a real glyph at code 32 with a wide width
    // (651/1000) — it is not a space. That width must not raise the run-gap
    // threshold above a normal 0.25 em word space, or a font switch into
    // math glues words ("of dk"). Same geometry as the gap test above, but
    // /F2 declares FirstChar 32 with a 651-wide code-32 glyph.
    char font[4096];
    size_t pos = 0;
    appendf(font, sizeof(font), &pos,
            "<< /Type /Font /Subtype /Type1 /BaseFont /GHIJKL+FakeCMMI "
            "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 122 /Widths [651 ");
    for (int i = 33; i <= 122; i++) appendf(font, sizeof(font), &pos, "500 ");
    appendf(font, sizeof(font), &pos, "] >>");
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT, font, NULL,
        "BT /F1 10 Tf 1 0 0 1 72 700 Tm (values of) Tj "
        "/F2 10 Tf 1 0 0 1 114.5 700 Tm (d) Tj "
        "/F1 10 Tf 1 0 0 1 119.5 700 Tm (,) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "values of d,\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_bfchar_bfrange) {
    // ToUnicode beats /Encoding: bfchar (incl. multi-unit ligature target),
    // offset-form bfrange, and array-form bfrange.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "2 beginbfchar\n"
        "<44> <00660069>\n"
        "<45> <20AC>\n"
        "endbfchar\n"
        "2 beginbfrange\n"
        "<41> <43> <0391>\n"
        "<50> <51> [<0058> <0059>]\n"
        "endbfrange\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (ABCDE) Tj (PQ) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A->Α B->Β C->Γ D->fi E->€ P->X Q->Y
    ASSERT_EQ_STR(text, "\xce\x91\xce\x92\xce\x93" "fi" "\xe2\x82\xac" "XY\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_null_cp_skipped) {
    // A ToUnicode target of <0000> (.notdef in real generators) must be
    // skipped: an embedded NUL would truncate the returned C string and
    // hide all following page text.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "2 beginbfchar\n"
        "<41> <0000>\n"
        "<42> <0042>\n"
        "endbfchar\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AB) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "B\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_wide_range_with_point_fixes) {
    // "Big range + point fixes" layout: one wide bfrange plus many bfchar
    // corrections above its lo. A code covered only by the wide range must
    // still map through it, however many bfchars sit in between.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "1 beginbfrange\n<00> <FF> <0100>\nendbfrange\n"
        "12 beginbfchar\n"
        "<41> <0391>\n<42> <0392>\n<43> <0393>\n<44> <0394>\n"
        "<45> <0395>\n<46> <0396>\n<47> <0397>\n<48> <0398>\n"
        "<49> <0399>\n<4A> <039A>\n<4B> <039B>\n<4C> <039C>\n"
        "endbfchar\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AP) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A (0x41) hits the bfchar fix -> U+0391; P (0x50) is covered only by
    // the wide range -> U+0100 + 0x50 = U+0150.
    ASSERT_EQ_STR(text, "\xce\x91\xc5\x90\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_contents_array_concatenated) {
    // /Contents array streams concatenate with '\n' between (stream
    // boundaries are token boundaries), so the split (One) Tj still lexes.
    size_t len = 0;
    char *pdf = make_text_pdf_contents_array(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "One\nTwo\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_differences_encoding) {
    // /Differences over WinAnsi, glyph names resolved via the AGL subset.
    size_t len = 0;
    char *pdf = make_text_pdf(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding << /BaseEncoding /WinAnsiEncoding "
        "/Differences [65 /adieresis /germandbls 97 /Euro] >> >>",
        "BT /F1 12 Tf 72 700 Td (ABa) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "\xc3\xa4\xc3\x9f\xe2\x82\xac\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_form_xobject_recursion) {
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        "<< /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        TEXT_HELV_FONT,
        "/Type /XObject /Subtype /Form /BBox [0 0 200 200] "
        "/Resources << /Font << /F1 4 0 R >> >>",
        "BT /F1 12 Tf 10 100 Td (Inside form) Tj ET",
        "BT /F1 12 Tf 72 700 Td (Before) Tj ET "
        "q 1 0 0 1 50 50 cm /Fx Do Q "
        "BT /F1 12 Tf 72 600 Td (After) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Before\nInside form\nAfter\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_form_xobject_self_cycle_guarded) {
    // A form that draws itself must terminate via the cycle guard/depth cap.
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        "<< /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        TEXT_HELV_FONT,
        "/Type /XObject /Subtype /Form /BBox [0 0 200 200] "
        "/Resources << /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        "BT /F1 12 Tf 10 100 Td (Loop) Tj ET /Fx Do",
        "/Fx Do", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Loop") != NULL);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_identity_h_ttf_tounicode) {
    // The writer's own TTF path (CIDFontType2 + Identity-H + ToUnicode) must
    // round-trip UTF-8 text through 2-byte glyph codes.
    TspdfWriter *w = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(w,
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) {
        tspdf_writer_destroy(w);
        SKIP("LiberationSans-Regular.ttf not installed");
    }
    TTF_Font *ttf = tspdf_writer_get_ttf(w, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(w, fname);
    ASSERT(ttf != NULL);
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, fname, 14.0);
    tspdf_stream_text_position(page, 72, 700);
    tspdf_stream_show_text_utf8(page, "Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 done",
                                ttf, pdf_font);
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 done\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_cid_no_tounicode_replacement) {
    // Identity-H without /ToUnicode: one U+FFFD per 2-byte code, counted.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type0 /BaseFont /NoMap "
        "/Encoding /Identity-H /DescendantFonts [5 0 R] >>",
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /NoMap "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
        "/Supplement 0 >> /DW 1000 >>",
        NULL,
        "BT /F1 12 Tf 72 700 Td <00410042> Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfTextStats stats;
    const char *text = tspdf_reader_page_text_stats(doc, 0, &stats, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "\xef\xbf\xbd\xef\xbf\xbd\n");
    ASSERT_EQ_SIZE(stats.glyphs, 2);
    ASSERT_EQ_SIZE(stats.replacements, 2);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_encrypted_pdf) {
    TspdfWriter *w = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 12.0);
    tspdf_stream_text_position(page, 72, 700);
    tspdf_stream_show_text(page, "Secret text");
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                "pw", "owner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(pdf);

    TspdfReader *doc2 = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(doc2 != NULL);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Secret text\n");
    tspdf_reader_destroy(doc2);
    free(enc);
}

TEST(test_text_page_out_of_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 5, &err);
    ASSERT(text == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_text_no_bt_writer_style_content) {
    // The writer's own fixtures emit Tf/Tj without BT..ET; extraction must
    // still see the text (lenient like real viewers).
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Page 1\n");
    tspdf_reader_destroy(doc);
}

TEST(test_text_malformed_content_no_crash) {
    // Garbage operators, unbalanced constructs, truncated hex: no crash,
    // best-effort text.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (ok) Tj zz 3 qq [ (unterminated TJ "
        "\x01\x02\xff garbage ) ] TJ <4 broken ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "ok") != NULL);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_ligatures_folded_to_ascii) {
    // Unicode ligature code points (U+FB00-FB06) fold to their ASCII letter
    // sequences at emit time (matching poppler's Latin1 fold tables and
    // Unicode NFKC).
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "4 beginbfchar\n"
        "<41> <FB01>\n"
        "<42> <FB04>\n"
        "<43> <FB06>\n"
        "<44> <FB05>\n"
        "endbfchar\n"
        "1 beginbfrange\n"
        "<50> <52> <FB00>\n"
        "endbfrange\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AnBC.D) Tj 0 -20 Td (PQR) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A->U+FB01 "fi", B->U+FB04 "ffl", C->U+FB06 "st", D->U+FB05 "st"
    // (long s t, NFKC folds it to "st" just like FB06);
    // bfrange P,Q,R -> U+FB00 "ff", U+FB01 "fi", U+FB02 "fl".
    ASSERT_EQ_STR(text, "finfflst.st\nfffifl\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_trailing_spaces_trimmed) {
    // Trailing spaces at end of each output line are trimmed (poppler
    // behavior) — both mid-document and on the final line.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Hello   ) Tj 0 -20 Td (World ) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello\nWorld\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_two_columns) {
    // Two columns drawn out of visual order (right column of row 1 first).
    // Layout mode must re-order by x, keep both cells of a row on one line
    // separated by a run of spaces, and align the columns across rows.
    // Plain mode must stay in content-stream order, byte-identical.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 300 700 Td (XXX) Tj ET "
        "BT /F1 12 Tf 72 700 Td (AAA) Tj ET "
        "BT /F1 12 Tf 72 686 Td (BBB) Tj ET "
        "BT /F1 12 Tf 300 686 Td (YYY) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *plain = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(plain != NULL);
    ASSERT_EQ_STR(plain, "XXX AAA\nBBB YYY\n");

    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    char l1[256], l2[256];
    ASSERT(text_line(text, 0, l1, sizeof(l1)));
    ASSERT(text_line(text, 1, l2, sizeof(l2)));
    const char *a = strstr(l1, "AAA");
    const char *x = strstr(l1, "XXX");
    const char *b = strstr(l2, "BBB");
    const char *y = strstr(l2, "YYY");
    ASSERT(a && x && b && y);
    ASSERT(a < x);                       // re-ordered left-to-right
    ASSERT(x - a >= 3 + 5);              // a real gap, not one space
    ASSERT_EQ_SIZE((size_t)(x - l1), (size_t)(y - l2)); // columns align
    ASSERT_EQ_SIZE((size_t)(a - l1), (size_t)(b - l2));
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_table_grid) {
    // 3x2 table: three columns, two rows. Every column must start at the
    // same character offset in both rows.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Name) Tj ET "
        "BT /F1 12 Tf 252 700 Td (Qty) Tj ET "
        "BT /F1 12 Tf 432 700 Td (Price) Tj ET "
        "BT /F1 12 Tf 72 680 Td (Apple) Tj ET "
        "BT /F1 12 Tf 252 680 Td (3) Tj ET "
        "BT /F1 12 Tf 432 680 Td (1.50) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    char r1[512], r2[512];
    ASSERT(text_line(text, 0, r1, sizeof(r1)));
    ASSERT(text_line(text, 1, r2, sizeof(r2)));
    const char *qty = strstr(r1, "Qty");
    const char *price = strstr(r1, "Price");
    const char *three = strstr(r2, "3");
    const char *val = strstr(r2, "1.50");
    ASSERT(strstr(r1, "Name") == r1);
    ASSERT(strstr(r2, "Apple") == r2);
    ASSERT(qty && price && three && val);
    ASSERT_EQ_SIZE((size_t)(qty - r1), (size_t)(three - r2));
    ASSERT_EQ_SIZE((size_t)(price - r1), (size_t)(val - r2));
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_y_jitter_merges_line) {
    // Baselines 0.5pt apart (generator jitter) must land on one line.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Left) Tj ET "
        "BT /F1 12 Tf 200 699.5 Td (Right) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Left") != NULL);
    ASSERT(strstr(text, "Right") != NULL);
    size_t newlines = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') newlines++;
    ASSERT_EQ_SIZE(newlines, 1);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_cell_block_contiguous) {
    // Table-cell geometry from the syscard corpus: a label column with 14 pt
    // leading next to a content column with 16 pt leading drifts apart until
    // one visual row holds baselines 4.1 pt apart (0.37 em at 11 pt). Layout
    // mode must keep that row on one output line, and the merge must not
    // manufacture a blank line before the next row (the vertical gap is
    // measured from the merged line's lowest baseline, not its highest).
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 11 Tf 72 700 Td (Representations) Tj ET "
        "BT /F1 11 Tf 200 695.907 Td (exasperated) Tj ET "
        "BT /F1 11 Tf 200 679.735 Td (+0.39) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    char l1[256], l2[256];
    ASSERT(text_line(text, 0, l1, sizeof(l1)));
    ASSERT(text_line(text, 1, l2, sizeof(l2)));
    ASSERT(strstr(l1, "Representations") != NULL);
    ASSERT(strstr(l1, "exasperated") != NULL);
    ASSERT(strstr(l2, "+0.39") != NULL);
    size_t newlines = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') newlines++;
    ASSERT_EQ_SIZE(newlines, 2); // two rows, no blank line between them
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_blank_lines_bounded) {
    // A vertical gap beyond ~1.8 line heights becomes one blank line; a huge
    // gap is capped at two blank lines (never proportional to the distance).
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Top) Tj ET "
        "BT /F1 12 Tf 72 676 Td (Mid) Tj ET "
        "BT /F1 12 Tf 72 100 Td (Bottom) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Top\n\nMid\n\n\nBottom\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_hostile_x_clamped) {
    // A fragment at x = 1e9 must clamp to the grid width cap instead of
    // allocating a billion-column line (ASan run guards the allocation).
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (A) Tj ET "
        "BT /F1 12 Tf 1000000000 700 Td (B) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "A") != NULL);
    ASSERT(strstr(text, "B") != NULL);
    ASSERT(strlen(text) < 1200);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_empty_page) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT, "", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_page_out_of_range) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT, "", &len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 5, &err);
    ASSERT(text == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
    free(pdf);
}

void run_text_tests(void) {
    printf("\n  Text extraction:\n");
    RUN(test_text_simple_lines_writer_fixture);
    RUN(test_text_tj_kerning_spacing);
    RUN(test_text_multiline_td_tstar);
    RUN(test_text_xgap_emits_space);
    RUN(test_text_superscript_joins_line);
    RUN(test_text_close_lines_stay_distinct);
    RUN(test_text_tj_kern_space_missing_space_glyph);
    RUN(test_text_font_switch_gap_space);
    RUN(test_text_font_switch_bogus_space_width);
    RUN(test_text_tounicode_bfchar_bfrange);
    RUN(test_text_tounicode_null_cp_skipped);
    RUN(test_text_tounicode_wide_range_with_point_fixes);
    RUN(test_text_contents_array_concatenated);
    RUN(test_text_differences_encoding);
    RUN(test_text_form_xobject_recursion);
    RUN(test_text_form_xobject_self_cycle_guarded);
    RUN(test_text_identity_h_ttf_tounicode);
    RUN(test_text_cid_no_tounicode_replacement);
    RUN(test_text_encrypted_pdf);
    RUN(test_text_page_out_of_range);
    RUN(test_text_no_bt_writer_style_content);
    RUN(test_text_malformed_content_no_crash);
    RUN(test_text_ligatures_folded_to_ascii);
    RUN(test_text_trailing_spaces_trimmed);
    printf("\n  Layout-preserving text extraction:\n");
    RUN(test_text_layout_two_columns);
    RUN(test_text_layout_table_grid);
    RUN(test_text_layout_y_jitter_merges_line);
    RUN(test_text_layout_cell_block_contiguous);
    RUN(test_text_layout_blank_lines_bounded);
    RUN(test_text_layout_hostile_x_clamped);
    RUN(test_text_layout_empty_page);
    RUN(test_text_layout_page_out_of_range);
}
