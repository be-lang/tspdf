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

#include "../../src/util/pdftext.h"
#include "helpers.h"

static char *bm_make_three_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n")) goto fail;
    for (int i = 0; i < 3; i++) {
        off[3 + i] = pos;
        if (!appendf(pdf, 4096, &pos,
                     "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n",
                     3 + i)) goto fail;
    }
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *bm_make_rich_outline_pdf(size_t *out_len) {
    const size_t cap = 8192;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[10] = {0};
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n")) goto fail;
    for (int i = 0; i < 3; i++) {
        off[3 + i] = pos;
        if (!appendf(pdf, cap, &pos,
                     "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n",
                     3 + i)) goto fail;
    }
    off[6] = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Type /Outlines /First 7 0 R /Last 9 0 R /Count 2 >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, cap, &pos,
                 "7 0 obj\n<< /Title (Alpha) /Parent 6 0 R /Next 9 0 R /First 8 0 R "
                 "/Last 8 0 R /Count -1 /Dest [3 0 R /XYZ 72 700.5 1.5] "
                 "/C [1 0 0] /F 2 >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, cap, &pos,
                 "8 0 obj\n<< /Title (Alpha Sub) /Parent 7 0 R /Dest [4 0 R /FitH 500] >>\nendobj\n")) goto fail;
    off[9] = pos;
    if (!appendf(pdf, cap, &pos,
                 "9 0 obj\n<< /Title (Beta) /Parent 6 0 R /Prev 7 0 R "
                 "/A << /S /URI /URI (https://example.com/x) >> >>\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos, "xref\n0 10\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 9; i++) {
        if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, cap, &pos,
                 "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *md_make_xmp_pdf(size_t *out_len) {
    const char *xmp =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"></x:xmpmeta>"
        "<?xpacket end=\"w\"?>";
    const size_t cap = 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[5] = {0};
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata 4 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Type /Metadata /Subtype /XML /Length %zu >>\n"
                 "stream\n%s\nendstream\nendobj\n", strlen(xmp), xmp)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, cap, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *ser_make_dangling_inuse_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[6] = {0};
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Lang 5 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< >>\nendobj\n")) goto fail;
    off[5] = pos;  // in-use entry pointing at bytes that are not an object
    if (!appendf(pdf, 4096, &pos, "%% not an object, just junk bytes\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static TspdfReader *t2_bookmark_set_and_save(const char *title,
                                             uint8_t **out, size_t *out_len) {
    size_t pdf_len = 0;
    char *pdf = bm_make_three_page_pdf(&pdf_len);
    if (!pdf) return NULL;
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, pdf_len, &err);
    free(pdf);
    if (!doc) return NULL;

    TspdfBookmarkEntry entry = {title, 1, 0, 0, false, NULL};
    err = tspdf_reader_set_bookmarks(doc, &entry, 1);
    if (err != TSPDF_OK) { tspdf_reader_destroy(doc); return NULL; }

    *out = NULL; *out_len = 0;
    err = tspdf_reader_save_to_memory(doc, out, out_len);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) { free(*out); return NULL; }

    TspdfReader *re = tspdf_reader_open(*out, *out_len, &err);
    if (!re) { free(*out); return NULL; }
    return re;
}

TEST(test_bookmark_list_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    ASSERT_EQ_STR(bm[0].title, "OF-CH1");
    ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_SIZE(bm[0].page_index, 0);

    ASSERT_EQ_STR(bm[1].title, "OF-CH1-SUB");
    ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_SIZE(bm[1].page_index, 1);

    ASSERT_EQ_STR(bm[2].title, "OF-CH2");
    ASSERT_EQ_INT(bm[2].level, 1);
    ASSERT_EQ_SIZE(bm[2].page_index, 2);

    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_list_no_outline) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = (TspdfBookmarkInfo *)1;
    size_t n = 99;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);
    ASSERT(bm == NULL);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_three_level_roundtrip) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfBookmarkEntry entries[] = {
        {"Chapter 1", 1, 0, 0, false, NULL},
        {"Section 1.1", 2, 1, 0, false, NULL},
        {"Subsection 1.1.1", 3, 1, 0, false, NULL},
        {"Section 1.2", 2, 2, 0, false, NULL},
        {"Chapter 2", 1, 2, 0, false, NULL},
    };
    err = tspdf_reader_set_bookmarks(doc, entries, 5);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 5);
    ASSERT_EQ_STR(bm[0].title, "Chapter 1");   ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_STR(bm[1].title, "Section 1.1"); ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_STR(bm[2].title, "Subsection 1.1.1"); ASSERT_EQ_INT(bm[2].level, 3);
    ASSERT_EQ_STR(bm[3].title, "Section 1.2"); ASSERT_EQ_INT(bm[3].level, 2);
    ASSERT_EQ_STR(bm[4].title, "Chapter 2");   ASSERT_EQ_INT(bm[4].level, 1);
    ASSERT_EQ_SIZE(bm[4].page_index, 2);

    // Root /Count = 5 (all items open/visible).
    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *rc = tspdf_dict_get(root, "Count");
    ASSERT(rc && rc->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)rc->integer, 5);

    // Chapter 1 has /Count 3 (Section 1.1 + its subsection + Section 1.2).
    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "Chapter 1"));
    TspdfObj *c1c = tspdf_dict_get(ch1, "Count");
    ASSERT(c1c && c1c->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)c1c->integer, 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_nonascii_title_roundtrip) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *title = "Caf\xC3\xA9 \xE2\x86\x92 \xF0\x9F\x93\x84";  // "Café → 📄"
    TspdfBookmarkEntry entries[] = {{title, 1, 0, 0, false, NULL}};
    err = tspdf_reader_set_bookmarks(doc, entries, 1);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Stored as a UTF-16BE literal string: the BOM bytes serialize as the
    // octal escapes \376\377 (bytes > 126 are escaped by write_string_escaped).
    ASSERT(bytes_contains(out, out_len, "\\376\\377"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_validation_errors) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // First entry must be level 1.
    TspdfBookmarkEntry e_lvl2first[] = {{"X", 2, 0, 0, false, NULL}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_lvl2first, 1), TSPDF_ERR_INVALID_ARG);

    // Level jump 1 -> 3.
    TspdfBookmarkEntry e_jump[] = {{"A", 1, 0, 0, false, NULL}, {"B", 3, 0, 0, false, NULL}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_jump, 2), TSPDF_ERR_INVALID_ARG);

    // Empty title.
    TspdfBookmarkEntry e_empty[] = {{"", 1, 0, 0, false, NULL}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_empty, 1), TSPDF_ERR_INVALID_ARG);

    // Page out of range (only 3 pages: indices 0-2).
    TspdfBookmarkEntry e_page[] = {{"A", 1, 9, 0, false, NULL}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_page, 1), TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_clear) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_clear_bookmarks(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_clear_drops_pagemode_useoutlines) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_clear_bookmarks(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/UseOutlines"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_empty_clears) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_set_bookmarks(doc, NULL, 0);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);
    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_list_then_set_stable) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    TspdfBookmarkEntry *entries = (TspdfBookmarkEntry *)calloc(n, sizeof(*entries));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
    }
    err = tspdf_reader_set_bookmarks(doc, entries, n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    free(entries);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm2 = NULL;
    size_t n2 = 0;
    err = tspdf_reader_bookmarks(re, &bm2, &n2);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n2, 3);
    ASSERT_EQ_STR(bm2[0].title, "OF-CH1");     ASSERT_EQ_INT(bm2[0].level, 1);
    ASSERT_EQ_STR(bm2[1].title, "OF-CH1-SUB"); ASSERT_EQ_INT(bm2[1].level, 2);
    ASSERT_EQ_STR(bm2[2].title, "OF-CH2");     ASSERT_EQ_INT(bm2[2].level, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_list_cyclic_bounded) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_outline_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 2);   // LOOPA, LOOPB — no re-emission of the cycle
    ASSERT_EQ_STR(bm[0].title, "LOOPA");
    ASSERT_EQ_STR(bm[1].title, "LOOPB");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_large_bounded) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t N = 10000;
    TspdfBookmarkEntry *entries = (TspdfBookmarkEntry *)calloc(N, sizeof(*entries));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < N; i++) {
        entries[i].title = "item";
        entries[i].level = 1;
        entries[i].page_index = i % 3;
    }
    err = tspdf_reader_set_bookmarks(doc, entries, N);
    ASSERT_EQ_INT(err, TSPDF_OK);
    free(entries);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, N);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_list_indirect_title) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_SIZE(bm[0].page_index, 0);

    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_SIZE(bm[1].page_index, 1);

    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_INT(bm[2].level, 1);
    ASSERT_EQ_SIZE(bm[2].page_index, 1);

    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_add_preserves_indirect_titles) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    TspdfBookmarkEntry *entries =
        (TspdfBookmarkEntry *)calloc(n + 1, sizeof(TspdfBookmarkEntry));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
    }
    entries[n].title = "Appendix";
    entries[n].level = 1;
    entries[n].page_index = 0;
    err = tspdf_reader_set_bookmarks(doc, entries, n + 1);
    free(entries);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 4);
    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_STR(bm[3].title, "Appendix");

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_add_preserves_rich_dests) {
    size_t len = 0;
    char *pdf = bm_make_rich_outline_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);
    ASSERT(!bm[0].open);                          // Alpha collapsed (Count -1)
    ASSERT(bm[0].node != NULL);                   // preservation handle
    ASSERT_EQ_SIZE(bm[2].page_index, (size_t)-1); // URI action: no page

    TspdfBookmarkEntry entries[4];
    memset(entries, 0, sizeof(entries));
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
        entries[i].keep = bm[i].node;
    }
    entries[3].title = "Appendix";
    entries[3].level = 1;
    entries[3].page_index = 2;
    err = tspdf_reader_set_bookmarks(doc, entries, 4);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfBookmarkInfo *bm2 = NULL;
    size_t n2 = 0;
    err = tspdf_reader_bookmarks(re, &bm2, &n2);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n2, 4);
    ASSERT_EQ_STR(bm2[3].title, "Appendix");
    ASSERT(!bm2[0].open);                         // still collapsed
    ASSERT_EQ_SIZE(bm2[0].page_index, 0);         // XYZ dest still resolves

    // Raw structure: Alpha keeps its /XYZ dest, color, flags and collapse.
    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *rc = tspdf_dict_get(root, "Count");
    ASSERT(rc && rc->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)rc->integer, 3);  // Alpha collapsed: its child is hidden

    TspdfObj *alpha = dt_get(re, root, "First");
    ASSERT(alpha && dt_str_eq(tspdf_dict_get(alpha, "Title"), "Alpha"));
    TspdfObj *dest = dt_get(re, alpha, "Dest");
    ASSERT(dest && dest->type == TSPDF_OBJ_ARRAY && dest->array.count == 5);
    ASSERT(dest->array.items[0].type == TSPDF_OBJ_REF);
    ASSERT(dt_str_eq(&dest->array.items[1], "XYZ"));
    ASSERT(dest->array.items[2].type == TSPDF_OBJ_INT &&
           dest->array.items[2].integer == 72);
    ASSERT(dest->array.items[3].type == TSPDF_OBJ_REAL &&
           dest->array.items[3].real == 700.5);
    ASSERT(dest->array.items[4].type == TSPDF_OBJ_REAL &&
           dest->array.items[4].real == 1.5);
    TspdfObj *color = dt_get(re, alpha, "C");
    ASSERT(color && color->type == TSPDF_OBJ_ARRAY && color->array.count == 3);
    ASSERT(color->array.items[0].type == TSPDF_OBJ_INT &&
           color->array.items[0].integer == 1);
    TspdfObj *flags = dt_get(re, alpha, "F");
    ASSERT(flags && flags->type == TSPDF_OBJ_INT && flags->integer == 2);
    TspdfObj *acnt = dt_get(re, alpha, "Count");
    ASSERT(acnt && acnt->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)acnt->integer, -1);

    // Alpha Sub keeps its /FitH dest.
    TspdfObj *sub = dt_get(re, alpha, "First");
    ASSERT(sub && dt_str_eq(tspdf_dict_get(sub, "Title"), "Alpha Sub"));
    TspdfObj *sdest = dt_get(re, sub, "Dest");
    ASSERT(sdest && sdest->type == TSPDF_OBJ_ARRAY && sdest->array.count == 3);
    ASSERT(dt_str_eq(&sdest->array.items[1], "FitH"));
    ASSERT(sdest->array.items[2].type == TSPDF_OBJ_INT &&
           sdest->array.items[2].integer == 500);

    // Beta keeps its URI /A action and gains no synthesized /Dest.
    TspdfObj *beta = dt_get(re, alpha, "Next");
    ASSERT(beta && dt_str_eq(tspdf_dict_get(beta, "Title"), "Beta"));
    ASSERT(tspdf_dict_get(beta, "Dest") == NULL);
    TspdfObj *act = dt_get(re, beta, "A");
    ASSERT(act && act->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(act, "S"), "URI"));
    ASSERT(dt_str_eq(tspdf_dict_get(act, "URI"), "https://example.com/x"));

    // The new entry gets the usual [page /Fit] destination.
    TspdfObj *app = dt_get(re, beta, "Next");
    ASSERT(app && dt_str_eq(tspdf_dict_get(app, "Title"), "Appendix"));
    TspdfObj *adest = dt_get(re, app, "Dest");
    ASSERT(adest && adest->type == TSPDF_OBJ_ARRAY && adest->array.count == 2);
    ASSERT(dt_str_eq(&adest->array.items[1], "Fit"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_metadata_xmp_detected) {
    size_t len = 0;
    char *pdf = md_make_xmp_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT(tspdf_reader_has_xmp_metadata(doc));
    tspdf_reader_destroy(doc);
    free(pdf);

    char *plain = bm_make_three_page_pdf(&len);
    ASSERT(plain != NULL);
    doc = tspdf_reader_open((const uint8_t *)plain, len, &err);
    ASSERT(doc != NULL);
    ASSERT(!tspdf_reader_has_xmp_metadata(doc));
    tspdf_reader_destroy(doc);
    free(plain);
}

// --- XMP sync (tspdf_reader_sync_xmp_metadata) ---

// Assemble a full xpacket around `body` (the rdf:Description elements) with
// `pad` spaces of xpacket padding before the trailer. Returns malloc'd bytes.
static char *xmp_packet(const char *body, size_t pad, size_t *out_len) {
    const char *hdr =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    const char *mid = " </rdf:RDF>\n</x:xmpmeta>\n";
    const char *end = "<?xpacket end=\"w\"?>";
    size_t len = strlen(hdr) + strlen(body) + strlen(mid) + pad + strlen(end);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    size_t pos = 0;
    memcpy(p + pos, hdr, strlen(hdr)); pos += strlen(hdr);
    memcpy(p + pos, body, strlen(body)); pos += strlen(body);
    memcpy(p + pos, mid, strlen(mid)); pos += strlen(mid);
    memset(p + pos, ' ', pad); pos += pad;
    memcpy(p + pos, end, strlen(end)); pos += strlen(end);
    p[pos] = '\0';
    *out_len = pos;
    return p;
}

// A 1-page PDF whose catalog /Metadata stream holds `packet` (binary-safe),
// optionally Flate-compressed.
static uint8_t *xmp_make_pdf(const uint8_t *packet, size_t packet_len,
                             bool flate, size_t *out_len) {
    uint8_t *body = NULL;
    size_t body_len = packet_len;
    if (flate) {
        body = deflate_compress(packet, packet_len, &body_len);
        if (!body) return NULL;
    }
    size_t cap = 4096 + packet_len + body_len;
    char *pdf = (char *)malloc(cap);
    if (!pdf) { free(body); return NULL; }
    size_t pos = 0;
    size_t off[5] = {0};
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Metadata 4 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Type /Metadata /Subtype /XML%s /Length %zu >>\nstream\n",
                 flate ? " /Filter /FlateDecode" : "",
                 flate ? body_len : packet_len)) goto fail;
    if (pos + (flate ? body_len : packet_len) + 64 > cap) goto fail;
    memcpy(pdf + pos, flate ? body : packet, flate ? body_len : packet_len);
    pos += flate ? body_len : packet_len;
    if (!appendf(pdf, cap, &pos, "\nendstream\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, cap, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    free(body);
    *out_len = pos;
    return (uint8_t *)pdf;
fail:
    free(body);
    free(pdf);
    return NULL;
}

// Find the xpacket span (header through end of trailer) in a saved file.
// Returns false when absent.
static bool xmp_find_packet(const uint8_t *buf, size_t len,
                            size_t *start, size_t *pkt_len) {
    const char *hdr = "<?xpacket begin=";
    const char *end = "<?xpacket end=\"w\"?>";
    size_t hlen = strlen(hdr), elen = strlen(end);
    size_t hpos = (size_t)-1;
    for (size_t i = 0; i + hlen <= len; i++) {
        if (memcmp(buf + i, hdr, hlen) == 0) { hpos = i; break; }
    }
    if (hpos == (size_t)-1) return false;
    for (size_t i = len - elen + 1; i-- > hpos;) {
        if (memcmp(buf + i, end, elen) == 0) {
            *start = hpos;
            *pkt_len = i + elen - hpos;
            return true;
        }
    }
    return false;
}

static const char *XMP_TITLE_BODY =
    "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
    "   <dc:title>\n"
    "    <rdf:Alt>\n"
    "     <rdf:li xml:lang=\"x-default\">Old title</rdf:li>\n"
    "    </rdf:Alt>\n"
    "   </dc:title>\n"
    "  </rdf:Description>\n";

// dc:title (rdf:Alt x-default li) replacement: value swapped with XML
// escaping, the xpacket padding absorbs the length change (packet span byte
// count is unchanged), and the Info dict gets the same value.
TEST(test_xmp_sync_updates_dc_title) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "Ne\xC3\xBC & <Title>");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len,
        "<rdf:li xml:lang=\"x-default\">Ne\xC3\xBC &amp; &lt;Title&gt;</rdf:li>"));
    ASSERT(!bytes_contains(out, out_len, "Old title"));

    // Padding contract: the packet keeps its trailer and its total size.
    size_t pstart = 0, plen = 0;
    ASSERT(xmp_find_packet(out, out_len, &pstart, &plen));
    ASSERT_EQ_SIZE(plen, pkt_len);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(re), "Ne\xC3\xBC & <Title>");
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// The other property shapes: dc:creator (rdf:Seq li), dc:description
// (rdf:Alt), a simple text element under a non-conventional prefix bound to
// the pdf namespace, an attribute-form property on rdf:Description, and an
// empty-element simple property.
TEST(test_xmp_sync_all_property_forms) {
    const char *body =
        "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        "    xmlns:p=\"http://ns.adobe.com/pdf/1.3/\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        "    xmp:CreatorTool=\"Old Tool\">\n"
        "   <dc:creator><rdf:Seq><rdf:li>Old Author</rdf:li></rdf:Seq></dc:creator>\n"
        "   <dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">Old Desc</rdf:li></rdf:Alt></dc:description>\n"
        "   <p:Producer>Old Producer</p:Producer>\n"
        "   <p:Keywords/>\n"
        "  </rdf:Description>\n";
    size_t pkt_len = 0;
    char *pkt = xmp_packet(body, 400, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_author(doc, "Anne Author");
    tspdf_reader_set_subject(doc, "New \"Desc\"");
    tspdf_reader_set_producer(doc, "prod2");
    tspdf_reader_set_creator(doc, "tool<2>");
    tspdf_reader_set_keywords(doc, "k1, k2");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "<rdf:li>Anne Author</rdf:li>"));
    ASSERT(bytes_contains(out, out_len,
        "<rdf:li xml:lang=\"x-default\">New &quot;Desc&quot;</rdf:li>"));
    ASSERT(bytes_contains(out, out_len, "<p:Producer>prod2</p:Producer>"));
    ASSERT(bytes_contains(out, out_len, "xmp:CreatorTool=\"tool&lt;2&gt;\""));
    ASSERT(bytes_contains(out, out_len, "<p:Keywords>k1, k2</p:Keywords>"));
    ASSERT(!bytes_contains(out, out_len, "Old Author"));
    ASSERT(!bytes_contains(out, out_len, "Old Desc"));
    ASSERT(!bytes_contains(out, out_len, "Old Producer"));
    ASSERT(!bytes_contains(out, out_len, "Old Tool"));
    size_t pstart = 0, plen = 0;
    ASSERT(xmp_find_packet(out, out_len, &pstart, &plen));
    ASSERT_EQ_SIZE(plen, pkt_len);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// Fields whose property is missing from the packet (or whose value cannot be
// XML text, e.g. control characters) are reported back so the CLI can keep
// the stale-XMP notice for exactly those; the present fields still sync.
TEST(test_xmp_sync_reports_unsyncable_fields) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "Synced Title");
    tspdf_reader_set_keywords(doc, "kw");          // no pdf:Keywords in packet
    tspdf_reader_set_author(doc, "bad\x07value");  // control char: not XML text
    unsigned stale = tspdf_reader_sync_xmp_metadata(doc);
    ASSERT_EQ_INT((int)stale, (int)(TSPDF_XMP_KEYWORDS | TSPDF_XMP_AUTHOR));

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len,
        "<rdf:li xml:lang=\"x-default\">Synced Title</rdf:li>"));
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// A Flate-compressed packet is decoded, edited, and written back
// uncompressed (metadata streams stay scannable); the /Filter is dropped.
TEST(test_xmp_sync_flate_packet) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, true, &len);
    ASSERT(pdf != NULL);
    ASSERT(bytes_contains(pdf, len, "FlateDecode"));

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "Flate Title");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len,
        "<rdf:li xml:lang=\"x-default\">Flate Title</rdf:li>"));
    // This minimal file has no other stream, so no Flate may remain.
    ASSERT(!bytes_contains(out, out_len, "FlateDecode"));
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// When the new value exceeds the available padding the stream grows (the
// file is rewritten anyway) but the trailer must survive at the packet end.
TEST(test_xmp_sync_padding_grows_when_needed) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 1, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    char big[300];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, big);
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, big));
    size_t pstart = 0, plen = 0;
    ASSERT(xmp_find_packet(out, out_len, &pstart, &plen));
    ASSERT(plen > pkt_len);
    // The value sits inside the packet, before the trailer.
    ASSERT(bytes_contains(out + pstart, plen, big));
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// Clearing a field empties the XMP value the same way.
TEST(test_xmp_sync_clear_empties_value) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, NULL);
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(bytes_contains(out, out_len,
        "<rdf:li xml:lang=\"x-default\"></rdf:li>"));
    ASSERT(!bytes_contains(out, out_len, "Old title"));
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// A sync that edits the packet also refreshes xmp:ModifyDate, with the same
// timestamp the save then writes into Info /ModDate (ISO 8601 vs D: form).
TEST(test_xmp_sync_updates_modify_date) {
    const char *body =
        "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n"
        "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Old title</rdf:li></rdf:Alt></dc:title>\n"
        "   <xmp:ModifyDate>2001-01-01T00:00:00Z</xmp:ModifyDate>\n"
        "  </rdf:Description>\n";
    size_t pkt_len = 0;
    char *pkt = xmp_packet(body, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "Dated Title");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "2001-01-01T00:00:00Z"));

    // Read the fresh Info ModDate back and expect its ISO-8601 twin in XMP.
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *mod = tspdf_reader_get_mod_date(re);
    ASSERT(mod != NULL && strncmp(mod, "D:", 2) == 0 && strlen(mod) >= 16);
    char iso[64];
    snprintf(iso, sizeof(iso),
             "<xmp:ModifyDate>%.4s-%.2s-%.2sT%.2s:%.2s:%.2sZ</xmp:ModifyDate>",
             mod + 2, mod + 6, mod + 8, mod + 10, mod + 12, mod + 14);
    ASSERT(bytes_contains(out, out_len, iso));
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// A dc:creator Seq with several authors cannot be mapped onto the single
// Info string: the field is reported stale and the packet stays untouched.
TEST(test_xmp_sync_multi_li_bails) {
    const char *body =
        "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
        "   <dc:creator><rdf:Seq><rdf:li>First Author</rdf:li><rdf:li>Second Author</rdf:li></rdf:Seq></dc:creator>\n"
        "  </rdf:Description>\n";
    size_t pkt_len = 0;
    char *pkt = xmp_packet(body, 100, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_author(doc, "Solo Author");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc),
                  (int)TSPDF_XMP_AUTHOR);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    // Packet untouched, byte for byte.
    size_t pstart = 0, plen = 0;
    ASSERT(xmp_find_packet(out, out_len, &pstart, &plen));
    ASSERT_EQ_SIZE(plen, pkt_len);
    ASSERT(memcmp(out + pstart, pkt, pkt_len) == 0);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);
}

// Packets we cannot edit safely (UTF-16) are left alone; every requested
// field is reported stale.
TEST(test_xmp_sync_utf16_packet_bails) {
    const char ascii[] =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"></x:xmpmeta>"
        "<?xpacket end=\"w\"?>";
    // Expand to UTF-16BE.
    size_t alen = sizeof(ascii) - 1;
    uint8_t *wide = (uint8_t *)malloc(alen * 2 + 2);
    ASSERT(wide != NULL);
    wide[0] = 0xFE; wide[1] = 0xFF;
    for (size_t i = 0; i < alen; i++) {
        wide[2 + 2 * i] = 0;
        wide[3 + 2 * i] = (uint8_t)ascii[i];
    }
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf(wide, alen * 2 + 2, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "T");
    tspdf_reader_set_author(doc, "A");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc),
                  (int)(TSPDF_XMP_TITLE | TSPDF_XMP_AUTHOR));
    tspdf_reader_destroy(doc);
    free(pdf);
    free(wide);
}

// Without an XMP stream there is nothing stale: sync reports zero.
TEST(test_xmp_sync_without_packet_is_zero) {
    size_t len = 0;
    char *plain = bm_make_three_page_pdf(&len);
    ASSERT(plain != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)plain, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "T");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);
    tspdf_reader_destroy(doc);
    free(plain);
}

// Encrypted documents: the packet is decrypted before editing, and the save
// re-encrypts it (the plaintext value must not appear in the output bytes).
TEST(test_xmp_sync_encrypted_roundtrip) {
    size_t pkt_len = 0;
    char *pkt = xmp_packet(XMP_TITLE_BODY, 200, &pkt_len);
    ASSERT(pkt != NULL);
    size_t len = 0;
    uint8_t *pdf = xmp_make_pdf((const uint8_t *)pkt, pkt_len, false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                        "pw", "pw", 0xFFFFFFFC, 128),
                  TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(pkt);

    doc = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "EncTitleXYZQ");
    ASSERT_EQ_INT((int)tspdf_reader_sync_xmp_metadata(doc), 0);
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "EncTitleXYZQ"));  // stays encrypted
    tspdf_reader_destroy(doc);
    free(enc);

    // Decrypt-save the result: the synced value must be in the packet.
    doc = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(doc != NULL);
    TspdfSaveOptions opts = {0};
    opts.decrypt = true;
    opts.update_producer = true;
    uint8_t *dec = NULL;
    size_t dec_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_with_options(doc, &dec, &dec_len, &opts),
                  TSPDF_OK);
    ASSERT(bytes_contains(dec, dec_len,
        "<rdf:li xml:lang=\"x-default\">EncTitleXYZQ</rdf:li>"));
    free(dec);
    tspdf_reader_destroy(doc);
    free(out);
}

TEST(test_merge_copies_indirect_outline_strings) {
    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    TspdfReader *doc_b = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // No in-use classic xref entry may carry offset 0.
    ASSERT(!bytes_contains(out, out_len, "0000000000 00000 n"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 4);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 6);
    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_STR(bm[3].title, "Alpha");
    ASSERT_EQ_STR(bm[4].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[5].title, "Beta");
    ASSERT_EQ_SIZE(bm[3].page_index, 2);
    ASSERT_EQ_SIZE(bm[4].page_index, 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
}

TEST(test_serialize_never_writes_inuse_offset_zero) {
    size_t len = 0;
    char *pdf = ser_make_dangling_inuse_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // A classic-table in-use entry with offset 0 is exactly this line.
    ASSERT(!bytes_contains(out, out_len, "0000000000 00000 n"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_pdftext_bookmark_ascii_stays_raw) {
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_bookmark_set_and_save("Hello", &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(!bytes_contains(out, out_len, "\\376\\377"));

    TspdfBookmarkInfo *bm = NULL; size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, "Hello");

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_bookmark_latin1_plus_encodes_utf16be) {
    const char *title = "\xC3\xA9";
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_bookmark_set_and_save(title, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    ASSERT(bytes_contains(out, out_len, "\\000\\351"));

    TspdfBookmarkInfo *bm = NULL; size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_bookmark_cjk_encodes_utf16be) {
    const char *title = "\xE6\xBC\xA2";
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_bookmark_set_and_save(title, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    ASSERT(bytes_contains(out, out_len, "\\376\\377o\""));

    TspdfBookmarkInfo *bm = NULL; size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_bookmark_astral_encodes_surrogate_pair) {
    const char *title = "\xF0\x9F\x98\x80";
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_bookmark_set_and_save(title, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    ASSERT(bytes_contains(out, out_len, "\\376\\377\\330="));
    const uint8_t bom_surr[] = {
        '\\','3','7','6','\\','3','7','7',
        '\\','3','3','0','=',
        '\\','3','3','6','\\','0','0','0'
    };
    ASSERT(bytes_contains_bin(out, out_len, bom_surr, sizeof(bom_surr)));

    TspdfBookmarkInfo *bm = NULL; size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_bookmark_mixed_roundtrip) {
    const char *title = "A\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80";
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_bookmark_set_and_save(title, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));

    TspdfBookmarkInfo *bm = NULL; size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
}

void run_bookmark_tests(void) {
    printf("\n  Outline (bookmark) editing:\n");
    RUN(test_bookmark_list_fixture);
    RUN(test_bookmark_list_no_outline);
    RUN(test_bookmark_set_three_level_roundtrip);
    RUN(test_bookmark_set_nonascii_title_roundtrip);
    RUN(test_bookmark_set_validation_errors);
    RUN(test_bookmark_clear);
    RUN(test_bookmark_clear_drops_pagemode_useoutlines);
    RUN(test_bookmark_set_empty_clears);
    RUN(test_bookmark_list_then_set_stable);
    RUN(test_bookmark_list_cyclic_bounded);
    RUN(test_bookmark_set_large_bounded);
    RUN(test_bookmark_list_indirect_title);
    RUN(test_bookmark_add_preserves_indirect_titles);
    RUN(test_bookmark_add_preserves_rich_dests);
    RUN(test_metadata_xmp_detected);
    RUN(test_xmp_sync_updates_dc_title);
    RUN(test_xmp_sync_all_property_forms);
    RUN(test_xmp_sync_reports_unsyncable_fields);
    RUN(test_xmp_sync_flate_packet);
    RUN(test_xmp_sync_padding_grows_when_needed);
    RUN(test_xmp_sync_clear_empties_value);
    RUN(test_xmp_sync_updates_modify_date);
    RUN(test_xmp_sync_multi_li_bails);
    RUN(test_xmp_sync_utf16_packet_bails);
    RUN(test_xmp_sync_without_packet_is_zero);
    RUN(test_xmp_sync_encrypted_roundtrip);
    RUN(test_merge_copies_indirect_outline_strings);
    RUN(test_serialize_never_writes_inuse_offset_zero);
    printf("\n  Task 2: pdftext pin-down (bookmark title strings):\n");
    RUN(test_pdftext_bookmark_ascii_stays_raw);
    RUN(test_pdftext_bookmark_latin1_plus_encodes_utf16be);
    RUN(test_pdftext_bookmark_cjk_encodes_utf16be);
    RUN(test_pdftext_bookmark_astral_encodes_surrogate_pair);
    RUN(test_pdftext_bookmark_mixed_roundtrip);
}
