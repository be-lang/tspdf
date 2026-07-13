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

static bool bytes_contains(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return false;
    }

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static TspdfObj *test_resolve_ref(TspdfReader *doc, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num, doc->obj_cache, doc->crypt);
}

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

static char *make_catalog_features_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n"
                 "<< /Type /Catalog /Pages 2 0 R /PageMode /UseOutlines "
                 "/ViewerPreferences << /DisplayDocTitle true >> "
                 "/Outlines 4 0 R /Names 5 0 R /AcroForm 6 0 R "
                 "/Metadata 7 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Outlines /Count 0 >>\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Dests << /Names [] >> >>\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Fields [] /NeedAppearances true >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Type /Metadata /Subtype /XML /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
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

static TspdfObj *dt_catalog_get(TspdfReader *doc, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(doc->catalog, key));
}

static TspdfObj *dt_get(TspdfReader *doc, TspdfObj *dict, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(dict, key));
}

static bool dt_str_eq(TspdfObj *s, const char *want) {
    return s && (s->type == TSPDF_OBJ_STRING || s->type == TSPDF_OBJ_NAME) &&
           s->string.len == strlen(want) &&
           memcmp(s->string.data, want, s->string.len) == 0;
}

static char *dt_make_cyclic_outline_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Title (LOOPA) /Parent 4 0 R /Next 6 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (LOOPB) /Parent 4 0 R /Prev 5 0 R /Next 5 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

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

static bool bytes_contains_bin(const uint8_t *haystack, size_t haystack_len,
                                const uint8_t *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || needle_len > haystack_len)
        return false;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return true;
    }
    return false;
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
    RUN(test_merge_copies_indirect_outline_strings);
    RUN(test_serialize_never_writes_inuse_offset_zero);
    printf("\n  Task 2: pdftext pin-down (bookmark title strings):\n");
    RUN(test_pdftext_bookmark_ascii_stays_raw);
    RUN(test_pdftext_bookmark_latin1_plus_encodes_utf16be);
    RUN(test_pdftext_bookmark_cjk_encodes_utf16be);
    RUN(test_pdftext_bookmark_astral_encodes_surrogate_pair);
    RUN(test_pdftext_bookmark_mixed_roundtrip);
}
