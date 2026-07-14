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

static char *make_missing_stream_length_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< >>\nstream\nHello\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_duplicate_stream_length_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Length 1 /Length 5 >>\n"
                 "stream\nHello\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *meta_make_pdfdoc_info_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Title (caf\xE9) /Author (Plain Author) >>\nendobj\n")) goto fail;

    {
        size_t xref = pos;
        if (!appendf(pdf, 2048, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 4; i++) {
            if (!appendf(pdf, 2048, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, 2048, &pos,
                     "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static TspdfReader *infoplan_make_plain_source(uint8_t **bytes_out, size_t *len_out) {
    TspdfError err;
    TspdfReader *base = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    if (!base) return NULL;
    tspdf_reader_set_title(base, "Src Title");
    tspdf_reader_set_author(base, "Src Author");
    uint8_t *buf = NULL;
    size_t len = 0;
    /* update_producer off so the source Producer is a known non-tspdf value
     * we can distinguish from a later stamp; one_page.pdf ships (tspl). */
    TspdfSaveOptions o = tspdf_save_options_default();
    o.update_producer = false;
    err = tspdf_reader_save_to_memory_with_options(base, &buf, &len, &o);
    tspdf_reader_destroy(base);
    if (err != TSPDF_OK) { free(buf); return NULL; }
    TspdfReader *src = tspdf_reader_open(buf, len, &err);
    if (!src) { free(buf); return NULL; }
    *bytes_out = buf;
    *len_out = len;
    return src;
}

static TspdfReader *infoplan_make_encrypted_source(uint8_t **bytes_out, size_t *len_out) {
    TspdfError err;
    TspdfReader *base = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    if (!base) return NULL;
    tspdf_reader_set_title(base, "Src Title");
    tspdf_reader_set_author(base, "Src Author");
    uint8_t *buf = NULL;
    size_t len = 0;
    err = tspdf_reader_save_to_memory_encrypted(base, &buf, &len,
                                                "pw", "opw", 0xFFFFFFFC, 128);
    tspdf_reader_destroy(base);
    if (err != TSPDF_OK) { free(buf); return NULL; }
    TspdfReader *src = tspdf_reader_open_with_password(buf, len, "pw", &err);
    if (!src) { free(buf); return NULL; }
    *bytes_out = buf;
    *len_out = len;
    return src;
}

TEST(test_round_trip_save_reopen) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Save to memory
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out != NULL);
    ASSERT(out_len > 0);

    // Reopen the saved PDF
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    // Verify page attributes preserved
    for (size_t i = 0; i < 3; i++) {
        TspdfReaderPage *p1 = tspdf_reader_get_page(doc, i);
        TspdfReaderPage *p2 = tspdf_reader_get_page(doc2, i);
        ASSERT(p1->media_box[2] == p2->media_box[2]);
        ASSERT(p1->media_box[3] == p2->media_box[3]);
        ASSERT_EQ_INT(p1->rotate, p2->rotate);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_to_file) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_save(doc, "tests/data/round_trip.pdf");
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open_file("tests/data/round_trip.pdf", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    tspdf_reader_destroy(doc);
    remove("tests/data/round_trip.pdf");
}

TEST(test_save_adds_missing_stream_length) {
    size_t len = 0;
    char *pdf = make_missing_stream_length_page_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Length 5"));
    ASSERT(!bytes_contains(out, out_len, "<< >>\nstream\nHello"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_deduplicates_stream_length) {
    size_t len = 0;
    char *pdf = make_duplicate_stream_length_page_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(bytes_count(out, out_len, "/Length "), 1);
    ASSERT(bytes_contains(out, out_len, "/Length 5"));
    ASSERT(!bytes_contains(out, out_len, "/Length 1"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_metadata_set_and_read) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Test Title");
    tspdf_reader_set_author(doc, "Test Author");
    tspdf_reader_set_subject(doc, "Test Subject");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "Test Title");
    const char *author = tspdf_reader_get_author(doc2);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Test Author");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_remove_field) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Temp");
    tspdf_reader_set_title(doc, NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(tspdf_reader_get_title(doc2) == NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_preserved_through_manipulation) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "My Document");

    uint8_t *buf1 = NULL;
    size_t len1 = 0;
    err = tspdf_reader_save_to_memory(doc, &buf1, &len1);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(buf1, len1, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc2), "My Document");

    tspdf_reader_destroy(doc2);
    free(buf1);
    tspdf_reader_destroy(doc);
}

TEST(test_save_options_default_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Size "));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserve_object_ids) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Size "));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_strip_unused) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_use_xref_stream_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.use_xref_stream = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(bytes_contains(out, out_len, "/Size "));
    // W is right-sized from the actual max offset (2 bytes here) and max
    // field-3 value (no type-2 entries: 1 byte), not a fixed [1 8 2].
    ASSERT(bytes_contains(out, out_len, "/W [ 1 2 1 ]"));
    ASSERT(!bytes_contains(out, out_len, "/W [ 1 8 2 ]"));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);
    ASSERT(doc2->xref.count > 0);
    ASSERT(doc2->xref.entries[doc2->xref.count - 1].in_use);
    ASSERT(doc2->xref.entries[doc2->xref.count - 1].offset > 0);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserves_catalog_entries_with_strip_unused) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);

    TspdfObj *pages = tspdf_dict_get(doc2->catalog, "Pages");
    ASSERT(pages != NULL);
    ASSERT_EQ_INT(pages->type, TSPDF_OBJ_REF);
    ASSERT_EQ_INT(pages->ref.num, 2);

    TspdfObj *page_mode = tspdf_dict_get(doc2->catalog, "PageMode");
    ASSERT(page_mode != NULL);
    ASSERT_EQ_INT(page_mode->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)page_mode->string.data, "UseOutlines");

    TspdfObj *viewer = tspdf_dict_get(doc2->catalog, "ViewerPreferences");
    ASSERT(viewer != NULL);
    ASSERT_EQ_INT(viewer->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(viewer, "DisplayDocTitle") != NULL);

    TspdfObj *acro = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "AcroForm"));
    ASSERT(acro != NULL);
    ASSERT_EQ_INT(acro->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(acro, "Fields") != NULL);

    TspdfObj *names = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Names"));
    ASSERT(names != NULL);
    ASSERT_EQ_INT(names->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(names, "Dests") != NULL);

    TspdfObj *outlines = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Outlines"));
    ASSERT(outlines != NULL);
    ASSERT_EQ_INT(outlines->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(outlines, "Count") != NULL);

    TspdfObj *metadata = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Metadata"));
    ASSERT(metadata != NULL);
    ASSERT_EQ_INT(metadata->type, TSPDF_OBJ_STREAM);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_strip_metadata_removes_catalog_metadata) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Metadata"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Metadata") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "AcroForm") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_preserve_ids_with_strip_unused_keeps_catalog_root) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Root 1 0 R"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Pages") != NULL);
    ASSERT(test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "AcroForm")) != NULL);
    ASSERT(test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Metadata")) != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_preserve_ids_with_strip_metadata_rewrites_catalog) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Metadata"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Metadata") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "AcroForm") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_strip_metadata) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Should Be Removed");

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    // Title should be gone
    ASSERT(tspdf_reader_get_title(doc2) == NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_recompress) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_pdftext_cp1252_from_codepoint) {
    // ASCII and Latin-1 map to themselves.
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint('A'), 'A');
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0xFC), 0xFC);  // ü
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0xC4), 0xC4);  // Ä
    // cp1252 specials in the 0x80-0x9F window (not plain Latin-1).
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x20AC), 0x80); // €
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2013), 0x96); // en-dash
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2014), 0x97); // em-dash
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2022), 0x95); // bullet
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x201C), 0x93); // left curly quote
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0178), 0x9F); // Ÿ
    // No mapping: C1 controls, unassigned cp1252 slots, non-Latin scripts.
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0081), -1);
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0416), -1);  // Cyrillic Ж
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x6A5F), -1);  // CJK 機
}

TEST(test_pdftext_utf8_to_cp1252_strict) {
    char out[64];
    uint32_t bad = 0;
    // Umlauts and en-dash convert to the cp1252 bytes.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("Vertraulich \xe2\x80\x93 gepr\xc3\xbc"
                                       "ft", out, &bad),
                  TSPDF_PDFTEXT_OK);
    ASSERT_EQ_STR(out, "Vertraulich \x96 gepr\xfc" "ft");
    // Characters outside WinAnsi fail and name the offending code point.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("ok \xe6\xa9\x9f", out, &bad),
                  TSPDF_PDFTEXT_UNMAPPED);
    ASSERT_EQ_INT((int)bad, 0x6A5F);
    // Malformed UTF-8 is reported as such, not silently emitted.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("bad \xc3", out, &bad),
                  TSPDF_PDFTEXT_BAD_UTF8);
}

TEST(test_pdftext_utf8_to_cp1252_lossy) {
    char out[64];
    uint32_t bad = 0;
    // In-place conversion is allowed (output is never longer than input).
    char line[] = "na\xc3\xafve caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x98\x80!";
    size_t subs = tspdf_utf8_to_cp1252_lossy(line, line, &bad);
    ASSERT_EQ_SIZE(subs, 1);
    ASSERT_EQ_INT((int)bad, 0x1F600);  // the emoji became '?'
    ASSERT_EQ_STR(line, "na\xefve caf\xe9 \x97 ?!");
    // Pure WinAnsi text passes through with zero substitutions.
    ASSERT_EQ_SIZE(tspdf_utf8_to_cp1252_lossy("plain", out, &bad), 0);
    ASSERT_EQ_STR(out, "plain");
}

TEST(test_pdftext_utf16be_roundtrip) {
    // Encoder: non-ASCII Info strings become a BOM-prefixed UTF-16BE hex string.
    TspdfBuffer buf = tspdf_buffer_create(64);
    tspdf_pdftext_write_info_string(&buf, "Pr\xc3\xbc \xe2\x80\x93 \xf0\x9f\x98\x80");
    tspdf_buffer_append_byte(&buf, '\0');
    ASSERT_EQ_STR((const char *)buf.data, "<FEFF0050007200FC002020130020D83DDE00>");
    tspdf_buffer_destroy(&buf);

    // ASCII strings stay readable literal strings (with the usual escapes).
    TspdfBuffer buf2 = tspdf_buffer_create(64);
    tspdf_pdftext_write_info_string(&buf2, "Plain (Report)");
    tspdf_buffer_append_byte(&buf2, '\0');
    ASSERT_EQ_STR((const char *)buf2.data, "(Plain \\(Report\\))");
    tspdf_buffer_destroy(&buf2);

    // Decoder: BOM + UTF-16BE (incl. a surrogate pair) back to UTF-8.
    TspdfArena arena = tspdf_arena_create(1024);
    const uint8_t utf16[] = {0xFE, 0xFF, 0x00, 0x50, 0x00, 0xFC,
                             0x20, 0x13, 0xD8, 0x3D, 0xDE, 0x00};
    char *utf8 = tspdf_utf16be_to_utf8(utf16, sizeof(utf16), &arena);
    ASSERT(utf8 != NULL);
    ASSERT_EQ_STR(utf8, "P\xc3\xbc\xe2\x80\x93\xf0\x9f\x98\x80");
    // Strings without the BOM are not touched.
    ASSERT(tspdf_utf16be_to_utf8((const uint8_t *)"plain", 5, &arena) == NULL);
    tspdf_arena_destroy(&arena);
}

TEST(test_pdftext_pdfdoc_decode) {
    // ASCII and the Latin-1 rows map to themselves.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint('A'), 'A');
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xE9), 0xE9);    // é
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xFC), 0xFC);    // ü
    // PDFDocEncoding specials in 0x18-0x1F (accents) and 0x80-0xA0.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x18), 0x02D8);  // breve
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x1F), 0x02DC);  // small tilde
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x80), 0x2022);  // bullet
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x85), 0x2013);  // en-dash
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x8A), 0x2212);  // minus sign
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x93), 0xFB01);  // fi ligature
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x9E), 0x017E);  // ž
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xA0), 0x20AC);  // euro
    // Undefined slots decode to U+FFFD, never to garbage bytes.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x7F), 0xFFFD);
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x9F), 0xFFFD);
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xAD), 0xFFFD);
}

TEST(test_pdftext_text_string_decode) {
    TspdfArena arena = tspdf_arena_create(1024);

    // UTF-16BE with BOM (incl. a surrogate pair) decodes to UTF-8.
    const uint8_t utf16[] = {0xFE, 0xFF, 0x00, 0xFC, 0x00, 'n',
                             0xD8, 0x3D, 0xDE, 0x00};
    char *s = tspdf_pdf_text_to_utf8(utf16, sizeof(utf16), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "\xC3\xBCn\xF0\x9F\x98\x80");  // ü n 😀

    // No BOM + non-UTF-8 bytes: PDFDocEncoding ("caf\xE9" -> café).
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"caf\xE9", 4, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "caf\xC3\xA9");

    // PDFDocEncoding specials: 0x85 en-dash, 0xA0 euro, 0x9F undefined->U+FFFD.
    const uint8_t docenc[] = {'a', 0x85, 0xA0, 0x9F, 'z'};
    s = tspdf_pdf_text_to_utf8(docenc, sizeof(docenc), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "a\xE2\x80\x93\xE2\x82\xAC\xEF\xBF\xBDz");

    // Pure ASCII is returned unchanged.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"plain.txt", 9, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "plain.txt");

    // Bytes that already form valid non-ASCII UTF-8 (common non-conformant
    // writers) pass through instead of being mangled as PDFDocEncoding.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"caf\xC3\xA9", 5, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "caf\xC3\xA9");

    // UTF-8 BOM (PDF 2.0): stripped, payload validated.
    const uint8_t u8bom[] = {0xEF, 0xBB, 0xBF, 'o', 'k', 0xC3, 0xA9};
    s = tspdf_pdf_text_to_utf8(u8bom, sizeof(u8bom), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "ok\xC3\xA9");

    // Empty input decodes to the empty string, not NULL.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"", 0, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "");

    tspdf_arena_destroy(&arena);
}

TEST(test_pdftext_text_string_encode) {
    TspdfArena arena = tspdf_arena_create(1024);
    size_t len = 0;

    // Pure ASCII stays byte-identical (no BOM).
    uint8_t *b = tspdf_utf8_to_pdf_text("plain.txt", &len, &arena);
    ASSERT(b != NULL);
    ASSERT_EQ_SIZE(len, 9);
    ASSERT(memcmp(b, "plain.txt", 9) == 0);

    // Non-ASCII UTF-8 becomes UTF-16BE with BOM.
    b = tspdf_utf8_to_pdf_text("\xC3\xBCn\xC3\xAF" "code.txt", &len, &arena);
    ASSERT(b != NULL);
    const uint8_t want[] = {0xFE, 0xFF, 0x00, 0xFC, 0x00, 'n', 0x00, 0xEF,
                            0x00, 'c', 0x00, 'o', 0x00, 'd', 0x00, 'e',
                            0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't'};
    ASSERT_EQ_SIZE(len, sizeof(want));
    ASSERT(memcmp(b, want, sizeof(want)) == 0);

    // Astral code points use a surrogate pair.
    b = tspdf_utf8_to_pdf_text("\xF0\x9F\x98\x80", &len, &arena);  // 😀
    ASSERT(b != NULL);
    const uint8_t want2[] = {0xFE, 0xFF, 0xD8, 0x3D, 0xDE, 0x00};
    ASSERT_EQ_SIZE(len, sizeof(want2));
    ASSERT(memcmp(b, want2, sizeof(want2)) == 0);

    // Encode/decode round-trip through the codec.
    b = tspdf_utf8_to_pdf_text("sp \xC3\xBCn\xC3\xAF" "code.txt", &len, &arena);
    ASSERT(b != NULL);
    char *back = tspdf_pdf_text_to_utf8(b, len, &arena);
    ASSERT(back != NULL);
    ASSERT_EQ_STR(back, "sp \xC3\xBCn\xC3\xAF" "code.txt");

    // Invalid UTF-8 input is kept verbatim (never half-encoded).
    b = tspdf_utf8_to_pdf_text("bad\xC3", &len, &arena);
    ASSERT(b != NULL);
    ASSERT_EQ_SIZE(len, 4);
    ASSERT(memcmp(b, "bad\xC3", 4) == 0);

    tspdf_arena_destroy(&arena);
}

TEST(test_metadata_non_ascii_utf16_roundtrip) {
    // Non-ASCII metadata must be written as UTF-16BE with BOM (ISO 32000
    // text string), not raw UTF-8 bytes in a PDFDocEncoded literal.
    const char *title = "Pr\xc3\xbc" "fbericht \xe2\x80\x93 \xc3\x84nderungen";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, title);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Raw bytes: a BOM-prefixed hex string, and no octal-escaped UTF-8 tail.
    ASSERT(bytes_contains(out, out_len, "<FEFF"));
    ASSERT(!bytes_contains(out, out_len, "\\303\\274"));

    // Reader display path decodes the UTF-16BE string back to UTF-8.
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *got = tspdf_reader_get_title(doc2);
    ASSERT(got != NULL);
    ASSERT_EQ_STR(got, title);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_ascii_stays_literal) {
    // Pure-ASCII values keep the compact literal-string form.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Plain Report");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "(Plain Report)"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc2), "Plain Report");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_pdfdoc_encoded_decodes_to_utf8) {
    // A BOM-less Info string is PDFDocEncoding (ISO 32000 §7.9.2.2); the
    // getter must hand back UTF-8, never the raw 0xE9 byte.
    size_t len = 0;
    char *pdf = meta_make_pdfdoc_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *title = tspdf_reader_get_title(doc);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "caf\xC3\xA9");
    const char *author = tspdf_reader_get_author(doc);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Plain Author");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_producer_is_tspdf_with_version) {
    // Default save options set update_producer; the stamp must be the real
    // project name + version, not the leftover codename "(tspr)".
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "(tspr)"));
    ASSERT(bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserve_ids_producer_is_tspdf_with_version) {
    // The preserve-object-ids raw-copy path builds its own Info dict too.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "(tspr)"));
    ASSERT(bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_reader_save_to_memory_matches_file) {
    // The serializer stamps ModDate with second resolution, so a save pair
    // straddling a second boundary can legitimately differ; retry a few times
    // so the comparison cannot flake on that boundary.
    const char *tmp_path = "/tmp/tspdf_test_wasm_byte_identity.pdf";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    bool match = false;
    for (int attempt = 0; attempt < 3 && !match; attempt++) {
        err = tspdf_reader_save(doc, tmp_path);
        ASSERT_EQ_INT(err, TSPDF_OK);

        uint8_t *mem = NULL;
        size_t mem_len = 0;
        err = tspdf_reader_save_to_memory(doc, &mem, &mem_len);
        ASSERT_EQ_INT(err, TSPDF_OK);

        size_t file_len = 0;
        uint8_t *file_buf = wasm_read_whole_file(tmp_path, &file_len);
        ASSERT(file_buf != NULL);

        match = (file_len == mem_len) && (memcmp(file_buf, mem, mem_len) == 0);
        free(mem);
        free(file_buf);
    }
    remove(tmp_path);
    tspdf_reader_destroy(doc);
    ASSERT(match);
}

TEST(test_reader_save_to_memory_with_options_matches_file) {
    // strip_metadata removes the timestamped Info dict, so this pair is fully
    // deterministic and must compare equal on the first try.
    const char *tmp_path = "/tmp/tspdf_test_wasm_byte_identity_opts.pdf";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_metadata = true;
    opts.use_xref_stream = true;

    err = tspdf_reader_save_with_options(doc, tmp_path, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *mem = NULL;
    size_t mem_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &mem, &mem_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    size_t file_len = 0;
    uint8_t *file_buf = wasm_read_whole_file(tmp_path, &file_len);
    ASSERT(file_buf != NULL);

    ASSERT_EQ_SIZE(file_len, mem_len);
    ASSERT(memcmp(file_buf, mem, mem_len) == 0);

    free(mem);
    free(file_buf);
    remove(tmp_path);
    tspdf_reader_destroy(doc);
}

TEST(test_infoplan_plain_noedit_noproducer_carries) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_plain_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(src), "Src Title");

    TspdfSaveOptions o = tspdf_save_options_default();
    o.update_producer = false;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Src Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");
    ASSERT_EQ_STR(tspdf_reader_get_producer(doc), "tspl");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_plain_noedit_producer_stamps_keeps_fields) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_plain_source(&src_bytes, &src_len);
    ASSERT(src != NULL);

    TspdfSaveOptions o = tspdf_save_options_default();
    o.update_producer = true;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Src Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");
    ASSERT_EQ_STR(tspdf_reader_get_producer(doc), "tspdf " TSPDF_VERSION_STRING);

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_plain_edit_producer_merges) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_plain_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    tspdf_reader_set_title(src, "New Title");

    TspdfSaveOptions o = tspdf_save_options_default();
    o.update_producer = true;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "New Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_plain_strip_drops_info) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_plain_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    tspdf_reader_set_title(src, "New Title");

    TspdfSaveOptions o = tspdf_save_options_default();
    o.strip_metadata = true;
    o.update_producer = true;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Info "));

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT(tspdf_reader_get_title(doc) == NULL);
    ASSERT(tspdf_reader_get_author(doc) == NULL);

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_enc_noedit_carries_encrypted_strings) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_encrypted_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(src), "Src Title");

    /* Re-save (stays encrypted: opened with password, no decrypt). */
    TspdfSaveOptions o = tspdf_save_options_default();
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);
    /* Title text must not appear in plaintext in the encrypted file. */
    ASSERT(!bytes_contains(out, out_len, "Src Title"));

    TspdfReader *doc = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Src Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_enc_edit_merges_encrypted_strings) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_encrypted_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    tspdf_reader_set_title(src, "Secret Title");

    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_encrypted(src, &out, &out_len,
                                                           "pw2", "opw2", 0xFFFFFFFC, 256);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "Secret Title"));

    TspdfReader *doc = tspdf_reader_open_with_password(out, out_len, "pw2", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Secret Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_enc_decrypt_carries_plaintext) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_encrypted_source(&src_bytes, &src_len);
    ASSERT(src != NULL);

    TspdfSaveOptions o = tspdf_save_options_default();
    o.decrypt = true;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Src Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "Src Author");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

TEST(test_infoplan_enc_decrypt_edit_merges) {
    uint8_t *src_bytes = NULL; size_t src_len = 0;
    TspdfReader *src = infoplan_make_encrypted_source(&src_bytes, &src_len);
    ASSERT(src != NULL);
    tspdf_reader_set_author(src, "New Author");

    TspdfSaveOptions o = tspdf_save_options_default();
    o.decrypt = true;
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(src, &out, &out_len, &o);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc), "Src Title");
    ASSERT_EQ_STR(tspdf_reader_get_author(doc), "New Author");

    tspdf_reader_destroy(doc);
    free(out);
    tspdf_reader_destroy(src);
    free(src_bytes);
}

// --- Stream plan matrix (tspdf_stream_plan) ---
//
// tspdf_stream_plan reads a stream dict only; these tests build TspdfObj values
// on the stack (no arena) and assert the chosen action. They pin every branch
// of the six-branch decision that used to live inline in the writer, including
// the indirect-/DecodeParms armor-strip refusal (a past shipped-bug fix).

// A name object aliasing a C string literal (never mutated by stream_plan).
static TspdfObj sp_name(const char *s) {
    TspdfObj o = {0};
    o.type = TSPDF_OBJ_NAME;
    o.string.data = (uint8_t *)s;
    o.string.len = strlen(s);
    return o;
}

// A stream object wrapping the given dict.
static TspdfObj sp_stream(TspdfObj *dict) {
    TspdfObj o = {0};
    o.type = TSPDF_OBJ_STREAM;
    o.stream.dict = dict;
    return o;
}

// Build a dict from up to 4 (key, value) pairs; unused pairs pass key=NULL.
static TspdfObj sp_dict(TspdfDictEntry *entries, size_t count) {
    TspdfObj o = {0};
    o.type = TSPDF_OBJ_DICT;
    o.dict.entries = entries;
    o.dict.count = count;
    return o;
}

TEST(test_streamplan_recompress_off_keeps) {
    TspdfObj flate = sp_name("FlateDecode");
    TspdfDictEntry e[] = {{ (char *)"Filter", &flate }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    // recompress=false must always KEEP, even for a FlateDecode stream.
    TspdfStreamPlan p = tspdf_stream_plan(&stm, false, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_xref_stream_keeps) {
    TspdfObj flate = sp_name("FlateDecode");
    TspdfDictEntry e[] = {{ (char *)"Filter", &flate }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    // is_xref=true (an XRef stream) is never recompressed.
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, true, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_flate_redeflates) {
    TspdfObj flate = sp_name("FlateDecode");
    TspdfDictEntry e[] = {{ (char *)"Filter", &flate }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_REDEFLATE);
    ASSERT(!p.use_fast_level);  // not an image
}

TEST(test_streamplan_image_flate_uses_fast_level) {
    TspdfObj flate = sp_name("FlateDecode");
    TspdfObj img = sp_name("Image");
    TspdfDictEntry e[] = {
        { (char *)"Filter", &flate },
        { (char *)"Subtype", &img },
    };
    TspdfObj dict = sp_dict(e, 2);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_REDEFLATE);
    ASSERT(p.use_fast_level);  // image XObject gets the fast level
}

TEST(test_streamplan_unfiltered_adds_flate) {
    TspdfObj dict = sp_dict(NULL, 0);  // no /Filter at all
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_ADD_FLATE);
}

TEST(test_streamplan_unfiltered_empty_keeps) {
    TspdfObj dict = sp_dict(NULL, 0);
    TspdfObj stm = sp_stream(&dict);
    // Zero-length unfiltered stream is never compressed.
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 0);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_unfiltered_xmp_metadata_keeps) {
    TspdfObj meta = sp_name("Metadata");
    TspdfDictEntry e[] = {{ (char *)"Type", &meta }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    // XMP metadata streams stay uncompressed (PDF/A-style scanning).
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_unfiltered_with_decodeparms_keeps) {
    TspdfObj parms = sp_dict(NULL, 0);
    TspdfDictEntry e[] = {{ (char *)"DecodeParms", &parms }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    // An unfiltered stream carrying /DecodeParms is left verbatim.
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_armor_hex_strips) {
    TspdfObj hex = sp_name("ASCIIHexDecode");
    TspdfDictEntry e[] = {{ (char *)"Filter", &hex }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_ARMOR_STRIP);
}

TEST(test_streamplan_flate_array_strips) {
    // Array form [/FlateDecode] normalizes to a bare Flate via armor-strip.
    TspdfObj flate = sp_name("FlateDecode");
    TspdfObj arr = {0};
    arr.type = TSPDF_OBJ_ARRAY;
    arr.array.items = &flate;
    arr.array.count = 1;
    TspdfDictEntry e[] = {{ (char *)"Filter", &arr }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_ARMOR_STRIP);
}

TEST(test_streamplan_armor_with_direct_decodeparms_strips) {
    // ASCIIHex armor over Flate with a DIRECT /DecodeParms dict: the strip is
    // allowed because the decoder can see the parameters.
    TspdfObj hex = sp_name("ASCIIHexDecode");
    TspdfObj flate = sp_name("FlateDecode");
    TspdfObj filters[] = { hex, flate };
    TspdfObj arr = {0};
    arr.type = TSPDF_OBJ_ARRAY;
    arr.array.items = filters;
    arr.array.count = 2;
    TspdfObj parms = sp_dict(NULL, 0);   // direct dict
    TspdfDictEntry e[] = {
        { (char *)"Filter", &arr },
        { (char *)"DecodeParms", &parms },
    };
    TspdfObj dict = sp_dict(e, 2);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_ARMOR_STRIP);
}

TEST(test_streamplan_armor_indirect_decodeparms_refuses) {
    // PIN: ASCIIHex armor with an INDIRECT /DecodeParms (a ref the decoder
    // cannot resolve) must REFUSE the strip and keep the stream verbatim.
    // Baking wrongly-decoded bytes into plain Flate was a shipped bug.
    TspdfObj hex = sp_name("ASCIIHexDecode");
    TspdfObj ref = {0};
    ref.type = TSPDF_OBJ_REF;
    ref.ref.num = 6;
    ref.ref.gen = 0;
    TspdfDictEntry e[] = {
        { (char *)"Filter", &hex },
        { (char *)"DecodeParms", &ref },
    };
    TspdfObj dict = sp_dict(e, 2);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_armor_indirect_decodeparms_array_refuses) {
    // Same refusal when the indirect ref hides inside the /DecodeParms array.
    TspdfObj hex = sp_name("ASCIIHexDecode");
    TspdfObj flate = sp_name("FlateDecode");
    TspdfObj filters[] = { hex, flate };
    TspdfObj farr = {0};
    farr.type = TSPDF_OBJ_ARRAY;
    farr.array.items = filters;
    farr.array.count = 2;
    TspdfObj ref = {0};
    ref.type = TSPDF_OBJ_REF;
    ref.ref.num = 9;
    TspdfObj null_parm = {0};
    null_parm.type = TSPDF_OBJ_NULL;
    TspdfObj parr_items[] = { null_parm, ref };
    TspdfObj parr = {0};
    parr.type = TSPDF_OBJ_ARRAY;
    parr.array.items = parr_items;
    parr.array.count = 2;
    TspdfDictEntry e[] = {
        { (char *)"Filter", &farr },
        { (char *)"DecodeParms", &parr },
    };
    TspdfObj dict = sp_dict(e, 2);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

TEST(test_streamplan_lossy_filter_keeps) {
    // A DCTDecode (JPEG) stream is a lossy filter: never touched.
    TspdfObj dct = sp_name("DCTDecode");
    TspdfDictEntry e[] = {{ (char *)"Filter", &dct }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);
    TspdfStreamPlan p = tspdf_stream_plan(&stm, true, false, 100);
    ASSERT_EQ_INT(p.action, TSPDF_STREAM_KEEP);
}

/* --- Crypt-adapter seam (r3 task A) -------------------------------------
 * The one serializer emits strings, stream payloads, the /Encrypt dict, and
 * the trailer /ID through a TspdfCryptAdapter. The identity adapter is the
 * plain save; the crypt adapter encrypts with the per-object key. These
 * matrix tests pin the seam at byte level: the pinned vectors are the exact
 * bytes the pre-unification encrypted path emitted. */

static TspdfCrypt ca_rc4_crypt(void) {
    TspdfCrypt c = {0};
    const uint8_t key[5] = {0x01, 0x23, 0x45, 0x67, 0x89};
    memcpy(c.file_key, key, sizeof(key));
    c.key_len = 5;
    c.version = 2;
    c.revision = 3;
    c.use_aes = false;
    c.encrypt_metadata = true;
    return c;
}

TEST(test_cryptadapter_identity_string_is_escaped_literal) {
    TspdfCryptAdapter ad = tspr_crypt_adapter_identity();
    ASSERT(ad.crypt == NULL);

    TspdfBuffer buf = tspdf_buffer_create(64);
    TspdfBuffer ref = tspdf_buffer_create(64);
    const uint8_t s[] = "He(l\\l)o";
    ad.write_string(&ad, &buf, s, sizeof(s) - 1, 7, 0);
    tspr_write_string_escaped(&ref, s, sizeof(s) - 1);

    ASSERT_EQ_SIZE(buf.len, ref.len);
    ASSERT(memcmp(buf.data, ref.data, buf.len) == 0);
    tspdf_buffer_destroy(&buf);
    tspdf_buffer_destroy(&ref);
}

TEST(test_cryptadapter_identity_stream_untouched) {
    TspdfCryptAdapter ad = tspr_crypt_adapter_identity();
    TspdfObj dict = sp_dict(NULL, 0);
    TspdfObj stm = sp_stream(&dict);
    size_t out_len = 123;
    uint8_t *out = ad.transform_stream(&ad, &stm, 9, 0,
                                       (const uint8_t *)"stream-bytes", 12, &out_len);
    // NULL means "use the input unchanged" — the plain writer never copies.
    ASSERT(out == NULL);
    // The identity adapter contributes no /Encrypt object and no trailer /ID.
    ASSERT(ad.emit_encrypt_dict == NULL);
    ASSERT(ad.emit_trailer_id == NULL);
}

TEST(test_cryptadapter_rc4_string_pinned) {
    TspdfCrypt c = ca_rc4_crypt();
    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    ASSERT(ad.crypt == &c);

    TspdfBuffer buf = tspdf_buffer_create(64);
    ad.write_string(&ad, &buf, (const uint8_t *)"Hello", 5, 7, 0);
    // RC4 with the object key derived for obj 7 gen 0 from the 40-bit file
    // key 0123456789, hex-encoded — byte-for-byte what the old path wrote.
    ASSERT_EQ_SIZE(buf.len, strlen("<3740C3E979>"));
    ASSERT(memcmp(buf.data, "<3740C3E979>", buf.len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_cryptadapter_rc4_string_key_is_per_object) {
    TspdfCrypt c = ca_rc4_crypt();
    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);

    TspdfBuffer b7 = tspdf_buffer_create(64);
    TspdfBuffer b8 = tspdf_buffer_create(64);
    ad.write_string(&ad, &b7, (const uint8_t *)"Hello", 5, 7, 0);
    ad.write_string(&ad, &b8, (const uint8_t *)"Hello", 5, 8, 0);
    // Same plaintext, different object number: different object key.
    ASSERT(b7.len == b8.len && memcmp(b7.data, b8.data, b7.len) != 0);
    tspdf_buffer_destroy(&b7);
    tspdf_buffer_destroy(&b8);
}

static int ca_hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

TEST(test_cryptadapter_aes_string_roundtrips) {
    TspdfCrypt c = {0};
    for (int i = 0; i < 16; i++) c.file_key[i] = (uint8_t)i;
    c.key_len = 16;
    c.version = 4;
    c.revision = 4;
    c.use_aes = true;
    c.encrypt_metadata = true;

    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    TspdfBuffer buf = tspdf_buffer_create(128);
    const uint8_t plain[] = "Secret title";
    ad.write_string(&ad, &buf, plain, sizeof(plain) - 1, 12, 0);

    // A hex string <...> whose ciphertext decrypts back with the same key.
    ASSERT(buf.len >= 2 && buf.data[0] == '<' && buf.data[buf.len - 1] == '>');
    uint8_t raw[128];
    size_t raw_len = 0;
    for (size_t i = 1; i + 1 < buf.len; i += 2) {
        int hi = ca_hex_nibble(buf.data[i]), lo = ca_hex_nibble(buf.data[i + 1]);
        ASSERT(hi >= 0 && lo >= 0);
        raw[raw_len++] = (uint8_t)((hi << 4) | lo);
    }
    // AES-CBC: 16-byte IV plus at least one padded block.
    ASSERT(raw_len >= 32);
    size_t dec_len = raw_len;
    ASSERT_EQ_INT(tspdf_crypt_decrypt_string(&c, 12, 0, raw, &dec_len), TSPDF_OK);
    ASSERT_EQ_SIZE(dec_len, sizeof(plain) - 1);
    ASSERT(memcmp(raw, plain, dec_len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_cryptadapter_rc4_stream_pinned) {
    TspdfCrypt c = ca_rc4_crypt();
    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    TspdfObj dict = sp_dict(NULL, 0);
    TspdfObj stm = sp_stream(&dict);

    size_t out_len = 0;
    uint8_t *out = ad.transform_stream(&ad, &stm, 9, 0,
                                       (const uint8_t *)"stream-bytes", 12, &out_len);
    ASSERT(out != NULL);
    // RC4 keystream for obj 9 gen 0 over "stream-bytes" (pinned vector).
    const uint8_t expect[12] = {0xEC, 0xA5, 0xCC, 0x98, 0xF2, 0xBE,
                                0x54, 0x1A, 0x9B, 0xAA, 0x6D, 0x71};
    ASSERT_EQ_SIZE(out_len, sizeof(expect));
    ASSERT(memcmp(out, expect, sizeof(expect)) == 0);
    free(out);
}

TEST(test_cryptadapter_stream_metadata_exemption) {
    // /EncryptMetadata false: the XMP metadata stream is stored in the clear
    // (ISO 32000 §7.6.3.2), so the adapter must leave it untouched.
    TspdfCrypt c = ca_rc4_crypt();
    c.encrypt_metadata = false;
    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);

    TspdfObj mtype = sp_name("Metadata");
    TspdfDictEntry e[] = {{ (char *)"Type", &mtype }};
    TspdfObj dict = sp_dict(e, 1);
    TspdfObj stm = sp_stream(&dict);

    size_t out_len = 0;
    uint8_t *out = ad.transform_stream(&ad, &stm, 9, 0,
                                       (const uint8_t *)"<xml/>", 6, &out_len);
    ASSERT(out == NULL);

    // With /EncryptMetadata true the same stream IS encrypted.
    c.encrypt_metadata = true;
    out = ad.transform_stream(&ad, &stm, 9, 0, (const uint8_t *)"<xml/>", 6, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(out_len, 6);
    free(out);
}

TEST(test_cryptadapter_encrypt_dict_v4_pinned) {
    TspdfCrypt c = {0};
    c.version = 4;
    c.revision = 4;
    c.use_aes = true;
    c.key_len = 16;
    c.O_len = 32;
    c.U_len = 32;
    for (int i = 0; i < 32; i++) c.O[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) c.U[i] = (uint8_t)(0x40 + i);
    c.permissions = 0xFFFFF0C0u;  /* -3904 as int32 */

    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    ASSERT(ad.emit_encrypt_dict != NULL);
    TspdfBuffer buf = tspdf_buffer_create(512);
    ad.emit_encrypt_dict(&ad, &buf, NULL, NULL);

    const char *expect =
        "<< /Filter /Standard /V 4 /R 4 /Length 128"
        " /CF << /StdCF << /CFM /AESV2 /Length 16 /AuthEvent /DocOpen >> >>"
        " /StmF /StdCF /StrF /StdCF"
        " /O <000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F>"
        " /U <404142434445464748494A4B4C4D4E4F505152535455565758595A5B5C5D5E5F>"
        " /P -3904 >>";
    ASSERT_EQ_SIZE(buf.len, strlen(expect));
    ASSERT(memcmp(buf.data, expect, buf.len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_cryptadapter_encrypt_dict_v5_pinned) {
    TspdfCrypt c = {0};
    c.version = 5;
    c.revision = 6;
    c.use_aes = true;
    c.key_len = 32;
    c.O_len = 48;
    c.U_len = 48;
    memset(c.O, 0x11, 48);
    memset(c.U, 0x22, 48);
    memset(c.OE, 0x33, 32);
    memset(c.UE, 0x44, 32);
    memset(c.Perms, 0x55, 16);
    c.permissions = 0xFFFFFFFCu;  /* -4 as int32 */

    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    TspdfBuffer buf = tspdf_buffer_create(1024);
    ad.emit_encrypt_dict(&ad, &buf, NULL, NULL);

    const char *expect =
        "<< /Filter /Standard /V 5 /R 6 /Length 256"
        " /CF << /StdCF << /CFM /AESV3 /Length 32 /AuthEvent /DocOpen >> >>"
        " /StmF /StdCF /StrF /StdCF"
        " /O <111111111111111111111111111111111111111111111111"
        "111111111111111111111111111111111111111111111111>"
        " /U <222222222222222222222222222222222222222222222222"
        "222222222222222222222222222222222222222222222222>"
        " /OE <3333333333333333333333333333333333333333333333333333333333333333>"
        " /UE <4444444444444444444444444444444444444444444444444444444444444444>"
        " /Perms <55555555555555555555555555555555>"
        " /P -4 >>";
    ASSERT_EQ_SIZE(buf.len, strlen(expect));
    ASSERT(memcmp(buf.data, expect, buf.len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_cryptadapter_encrypt_dict_preserves_source) {
    // A crypt opened from an existing file carries the source /Encrypt dict;
    // the adapter must copy it verbatim, strings as raw hex (never object-key
    // encrypted — the one dict the spec exempts).
    TspdfObj vnum = {0};
    vnum.type = TSPDF_OBJ_INT;
    vnum.integer = 2;
    TspdfObj ostr = {0};
    ostr.type = TSPDF_OBJ_STRING;
    ostr.string.data = (uint8_t *)"\x01\xFF";
    ostr.string.len = 2;
    TspdfObj fname = sp_name("Standard");
    TspdfDictEntry e[] = {
        { (char *)"Filter", &fname },
        { (char *)"V", &vnum },
        { (char *)"O", &ostr },
    };
    TspdfObj src = sp_dict(e, 3);

    TspdfCrypt c = ca_rc4_crypt();
    c.src_encrypt_dict = &src;

    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    TspdfBuffer buf = tspdf_buffer_create(128);
    ad.emit_encrypt_dict(&ad, &buf, NULL, NULL);

    const char *expect = "<< /Filter /Standard /V 2 /O <01FF> >>";
    ASSERT_EQ_SIZE(buf.len, strlen(expect));
    ASSERT(memcmp(buf.data, expect, buf.len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_cryptadapter_trailer_id_pinned) {
    uint8_t fid[16];
    for (int i = 0; i < 16; i++) fid[i] = (uint8_t)(0xA0 + i);
    TspdfCrypt c = ca_rc4_crypt();
    c.file_id = fid;
    c.file_id_len = sizeof(fid);

    TspdfCryptAdapter ad = tspr_crypt_adapter_encrypt(&c);
    ASSERT(ad.emit_trailer_id != NULL);
    TspdfBuffer buf = tspdf_buffer_create(256);
    tspdf_buffer_append_str(&buf, "seed");
    ad.emit_trailer_id(&ad, &buf);

    // First ID string: the crypt file id verbatim. Second: MD5 of the bytes
    // serialized so far ("seed"), so equal content stays byte-identical.
    uint8_t digest[16];
    md5_hash((const uint8_t *)"seed", 4, digest);
    char expect[256];
    size_t pos = 0;
    pos += (size_t)snprintf(expect + pos, sizeof(expect) - pos, "seed<");
    for (int i = 0; i < 16; i++)
        pos += (size_t)snprintf(expect + pos, sizeof(expect) - pos, "%02X", fid[i]);
    pos += (size_t)snprintf(expect + pos, sizeof(expect) - pos, "> <");
    for (int i = 0; i < 16; i++)
        pos += (size_t)snprintf(expect + pos, sizeof(expect) - pos, "%02X", digest[i]);
    pos += (size_t)snprintf(expect + pos, sizeof(expect) - pos, ">");

    ASSERT_EQ_SIZE(buf.len, pos);
    ASSERT(memcmp(buf.data, expect, buf.len) == 0);
    tspdf_buffer_destroy(&buf);
}

TEST(test_encrypted_save_ignores_size_options) {
    // Difference pin: encrypted saves ignore the size-focused save options.
    // recompress_streams / use_xref_stream must not trigger ObjStm packing or
    // an xref stream when the (classic) input didn't use object streams.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                "pw", "pw", 0xFFFFFFFC, 128);
    tspdf_reader_destroy(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *opened = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(opened != NULL);
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    opts.use_xref_stream = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(opened, &out, &out_len, &opts);
    tspdf_reader_destroy(opened);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(bytes_contains(out, out_len, "/Encrypt "));
    ASSERT(!bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(!bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(bytes_contains(out, out_len, "xref\n0 "));

    TspdfReader *reopened = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);
    tspdf_reader_destroy(reopened);
    free(out);
    free(enc);
}

TEST(test_encrypted_save_repacks_objstm_input) {
    // Difference pin: an encrypted save DOES pack object streams when the
    // input stored objects that way (preserve-style rule, shared with plain).
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;  // packs into ObjStm
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &packed, &packed_len, &opts);
    tspdf_reader_destroy(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(packed, packed_len, "/Type /ObjStm"));

    TspdfReader *pdoc = tspdf_reader_open(packed, packed_len, &err);
    ASSERT(pdoc != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(pdoc, &enc, &enc_len,
                                                "pw", "pw", 0xFFFFFFFC, 128);
    tspdf_reader_destroy(pdoc);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(enc, enc_len, "/Type /ObjStm"));
    ASSERT(bytes_contains(enc, enc_len, "/Type /XRef"));

    // And the preserved-encryption resave keeps packing.
    TspdfReader *opened = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(opened != NULL);
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(opened, &out, &out_len);
    tspdf_reader_destroy(opened);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));

    TspdfReader *reopened = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);
    tspdf_reader_destroy(reopened);
    free(out);
    free(enc);
    free(packed);
}

void run_serialize_tests(void) {
    printf("\n  Serialization:\n");
    RUN(test_round_trip_save_reopen);
    RUN(test_save_to_file);
    RUN(test_save_adds_missing_stream_length);
    RUN(test_save_deduplicates_stream_length);
    printf("\n  Metadata:\n");
    RUN(test_metadata_set_and_read);
    RUN(test_metadata_remove_field);
    RUN(test_metadata_preserved_through_manipulation);
    printf("\n  Save options:\n");
    RUN(test_save_options_default_roundtrip);
    RUN(test_save_preserve_object_ids);
    RUN(test_save_strip_unused);
    RUN(test_save_use_xref_stream_roundtrip);
    RUN(test_save_preserves_catalog_entries_with_strip_unused);
    RUN(test_save_strip_metadata_removes_catalog_metadata);
    RUN(test_save_preserve_ids_with_strip_unused_keeps_catalog_root);
    RUN(test_save_preserve_ids_with_strip_metadata_rewrites_catalog);
    RUN(test_save_strip_metadata);
    RUN(test_save_recompress);
    printf("\n  Encoding/i18n:\n");
    RUN(test_pdftext_cp1252_from_codepoint);
    RUN(test_pdftext_utf8_to_cp1252_strict);
    RUN(test_pdftext_utf8_to_cp1252_lossy);
    RUN(test_pdftext_utf16be_roundtrip);
    RUN(test_pdftext_pdfdoc_decode);
    RUN(test_pdftext_text_string_decode);
    RUN(test_pdftext_text_string_encode);
    RUN(test_metadata_non_ascii_utf16_roundtrip);
    RUN(test_metadata_ascii_stays_literal);
    RUN(test_metadata_pdfdoc_encoded_decodes_to_utf8);
    RUN(test_save_producer_is_tspdf_with_version);
    RUN(test_save_preserve_ids_producer_is_tspdf_with_version);
    printf("\n  Save-to-memory byte identity (wasm):\n");
    RUN(test_reader_save_to_memory_matches_file);
    RUN(test_reader_save_to_memory_with_options_matches_file);
    printf("\n  Info plan matrix:\n");
    RUN(test_infoplan_plain_noedit_noproducer_carries);
    RUN(test_infoplan_plain_noedit_producer_stamps_keeps_fields);
    RUN(test_infoplan_plain_edit_producer_merges);
    RUN(test_infoplan_plain_strip_drops_info);
    RUN(test_infoplan_enc_noedit_carries_encrypted_strings);
    RUN(test_infoplan_enc_edit_merges_encrypted_strings);
    RUN(test_infoplan_enc_decrypt_carries_plaintext);
    RUN(test_infoplan_enc_decrypt_edit_merges);
    printf("\n  Stream plan matrix:\n");
    RUN(test_streamplan_recompress_off_keeps);
    RUN(test_streamplan_xref_stream_keeps);
    RUN(test_streamplan_flate_redeflates);
    RUN(test_streamplan_image_flate_uses_fast_level);
    RUN(test_streamplan_unfiltered_adds_flate);
    RUN(test_streamplan_unfiltered_empty_keeps);
    RUN(test_streamplan_unfiltered_xmp_metadata_keeps);
    RUN(test_streamplan_unfiltered_with_decodeparms_keeps);
    RUN(test_streamplan_armor_hex_strips);
    RUN(test_streamplan_flate_array_strips);
    RUN(test_streamplan_armor_with_direct_decodeparms_strips);
    RUN(test_streamplan_armor_indirect_decodeparms_refuses);
    RUN(test_streamplan_armor_indirect_decodeparms_array_refuses);
    RUN(test_streamplan_lossy_filter_keeps);

    printf("\n  Crypt adapter seam:\n");
    RUN(test_cryptadapter_identity_string_is_escaped_literal);
    RUN(test_cryptadapter_identity_stream_untouched);
    RUN(test_cryptadapter_rc4_string_pinned);
    RUN(test_cryptadapter_rc4_string_key_is_per_object);
    RUN(test_cryptadapter_aes_string_roundtrips);
    RUN(test_cryptadapter_rc4_stream_pinned);
    RUN(test_cryptadapter_stream_metadata_exemption);
    RUN(test_cryptadapter_encrypt_dict_v4_pinned);
    RUN(test_cryptadapter_encrypt_dict_v5_pinned);
    RUN(test_cryptadapter_encrypt_dict_preserves_source);
    RUN(test_cryptadapter_trailer_id_pinned);
    RUN(test_encrypted_save_ignores_size_options);
    RUN(test_encrypted_save_repacks_objstm_input);
}
