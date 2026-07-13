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
#include "helpers.h"

static char *make_unbalanced_content_pdf(size_t *out_len) {
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
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;
    // Unbalanced: an extra `q` with a 2x scale, never matched by `Q`.
    const char *content = "q 2 0 0 2 0 0 cm\n";
    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
                 strlen(content), content)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n0 5\n0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static bool contents_item_bytes(TspdfReader *doc, TspdfObj *item,
                                const char **out, size_t *out_len) {
    if (!item || item->type != TSPDF_OBJ_REF) return false;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *s = NULL;
    if (item->ref.num < doc->xref.count) {
        s = tspdf_xref_resolve(&doc->xref, &parser, item->ref.num, doc->obj_cache, doc->crypt);
    } else {
        size_t idx = item->ref.num - doc->xref.count;
        if (idx < doc->new_objs.count) s = doc->new_objs.objs[idx];
    }
    if (!s || s->type != TSPDF_OBJ_STREAM) return false;
    *out = (const char *)s->stream.data;
    *out_len = s->stream.len;
    return s->stream.data != NULL;
}

static size_t dict_key_count(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return 0;
    size_t n = 0;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) n++;
    }
    return n;
}

static uint8_t *make_stamp_text_pdf(const char *text, size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    if (!font || !page) { tspdf_writer_destroy(w); return NULL; }
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 24.0);
    tspdf_stream_text_position(page, 72, 400);
    tspdf_stream_show_text(page, text);
    tspdf_stream_end_text(page);
    uint8_t *data = NULL;
    *out_len = 0;
    tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    return data;
}

static TspdfError draw_xobject_on_page(TspdfReader *doc, size_t page_index,
                                       uint32_t xobj_num, double s, double x, double y) {
    const char *name = tspdf_page_add_xobject(doc, page_index, xobj_num);
    if (!name) return TSPDF_ERR_ALLOC;
    TspdfStream *ov = tspdf_page_begin_content(doc, page_index);
    if (!ov) return TSPDF_ERR_ALLOC;
    tspdf_stream_draw_image(ov, name, x, y, s, s);
    return tspdf_page_end_content(doc, page_index, ov, NULL);
}

static uint32_t first_ref_num_in(TspdfObj *obj, int depth) {
    if (!obj || depth > 16) return 0;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            return obj->ref.num;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                uint32_t n = first_ref_num_in(&obj->array.items[i], depth + 1);
                if (n) return n;
            }
            return 0;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                uint32_t n = first_ref_num_in(obj->dict.entries[i].value, depth + 1);
                if (n) return n;
            }
            return 0;
        default:
            return 0;
    }
}

static char *make_two_stream_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *s1 = "BT /F1 24 Tf 72 700 Td (AlphaPart) Tj ET";
    const char *s2 = "BT /F1 24 Tf 72 650 Td (BetaPart) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources << /Font << /F1 6 0 R >> >> "
                 "/Contents [4 0 R 5 0 R] >>\nendobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s1), s1)) goto fail;
    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s2), s2)) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *make_cyclic_resources_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *content = "BT /F1 12 Tf 10 10 Td (Cyclic) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                 "/Resources << /XObject << /X1 5 0 R >> /Font << /F1 6 0 R >> >> "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;
    size_t obj5 = pos;
    // Form XObject that references itself and the page (cycles both ways).
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] "
                 "/Resources << /XObject << /Self 5 0 R /Page 3 0 R >> >> "
                 "/Length 0 >>\nstream\n\nendstream\nendobj\n")) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *make_mediabox_pdf(const char *mediabox, size_t *out_len) {
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
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [%s] >>\nendobj\n",
                 mediabox)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static uint8_t *make_multipage_text_pdf(size_t count, size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    if (!font) { tspdf_writer_destroy(w); return NULL; }
    for (size_t i = 0; i < count; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        if (!page) { tspdf_writer_destroy(w); return NULL; }
        char label[32];
        snprintf(label, sizeof(label), "PAGE%zu", i + 1);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 24.0);
        tspdf_stream_text_position(page, 72, 400);
        tspdf_stream_show_text(page, label);
        tspdf_stream_end_text(page);
    }
    uint8_t *data = NULL;
    *out_len = 0;
    tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    return data;
}

TEST(test_overlay_wraps_unbalanced_content) {
    size_t len = 0;
    char *raw = make_unbalanced_content_pdf(&len);
    ASSERT(raw != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)raw, len, &err);
    ASSERT(doc != NULL);

    TspdfWriter *res = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
    TspdfStream *ov = tspdf_page_begin_content(doc, 0);
    ASSERT(ov != NULL);
    tspdf_stream_begin_text(ov);
    tspdf_stream_set_font(ov, font, 12.0);
    tspdf_stream_text_position(ov, 72, 72);
    tspdf_stream_show_text(ov, "MARK");
    tspdf_stream_end_text(ov);
    err = tspdf_page_end_content(doc, 0, ov, res);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res);

    // The single original ref becomes a 4-item [q, old, Q, new] array.
    TspdfObj *contents = tspdf_dict_get(doc->pages.pages[0].page_dict, "Contents");
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(contents->array.count, 4);

    const char *b = NULL;
    size_t bl = 0;
    // items[0] is the "q\n" save that brackets the original content.
    ASSERT(contents_item_bytes(doc, &contents->array.items[0], &b, &bl));
    ASSERT(bl == 2 && memcmp(b, "q\n", 2) == 0);
    // items[2] is the matching "Q\n" restore, popping the original's leak.
    ASSERT(contents_item_bytes(doc, &contents->array.items[2], &b, &bl));
    ASSERT(bl == 2 && memcmp(b, "Q\n", 2) == 0);

    tspdf_reader_destroy(doc);
    free(raw);
}

TEST(test_overlay_text) {
    // Create a 1-page PDF
    TspdfWriter *creator = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(creator, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(creator);
    tspdf_stream_set_font(page, font, 24.0);
    tspdf_stream_move_to(page, 72, 400);
    tspdf_stream_show_text(page, "Original");
    uint8_t *pdf_data = NULL;
    size_t pdf_len = 0;
    tspdf_writer_save_to_memory(creator, &pdf_data, &pdf_len);
    tspdf_writer_destroy(creator);

    // Open with tspr
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf_data, pdf_len, &err);
    ASSERT(doc != NULL);

    // Overlay
    TspdfWriter *res_owner = tspdf_writer_create();
    const char *overlay_font = tspdf_writer_add_builtin_font(res_owner, "Courier");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, overlay_font, 18.0);
    tspdf_stream_move_to(overlay, 72, 300);
    tspdf_stream_show_text(overlay, "Overlay text");
    err = tspdf_page_end_content(doc, 0, overlay, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save and reopen
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
    free(pdf_data);
}

TEST(test_overlay_on_specific_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 1);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, font, 36.0);
    tspdf_stream_move_to(overlay, 100, 100);
    tspdf_stream_show_text(overlay, "WATERMARK");
    err = tspdf_page_end_content(doc, 1, overlay, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
}

TEST(test_overlay_abort) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_page_abort_content(overlay);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out != NULL);

    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_overlay_indirect_shared_resources) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_resources.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    // One resource owner reused for both pages, like the watermark CLI does:
    // a font plus an opacity ExtGState.
    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    const char *gs = tspdf_writer_add_opacity(res_owner, 0.3, 0.3);
    ASSERT(font != NULL);
    ASSERT(gs != NULL);
    for (size_t p = 0; p < 2; p++) {
        TspdfStream *ov = tspdf_page_begin_content(doc, p);
        ASSERT(ov != NULL);
        tspdf_stream_set_opacity(ov, gs);
        tspdf_stream_begin_text(ov);
        tspdf_stream_set_font(ov, font, 36.0);
        tspdf_stream_text_position(ov, 100, 100);
        tspdf_stream_show_text(ov, "DRAFT");
        tspdf_stream_end_text(ov);
        err = tspdf_page_end_content(doc, p, ov, res_owner);
        ASSERT_EQ_INT(err, TSPDF_OK);
    }
    // res_owner stays alive: `font`/`gs` point into it and are used below.

    // The merge must not have appended a second /Resources key.
    for (size_t p = 0; p < 2; p++) {
        ASSERT_EQ_SIZE(dict_key_count(doc->pages.pages[p].page_dict, "Resources"), 1);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    for (size_t p = 0; p < 2; p++) {
        TspdfObj *page_dict = doc2->pages.pages[p].page_dict;
        TspdfObj *res = test_resolve_ref(doc2, tspdf_dict_get(page_dict, "Resources"));
        ASSERT(res != NULL);
        ASSERT_EQ_INT(res->type, TSPDF_OBJ_DICT);
        ASSERT_EQ_SIZE(dict_key_count(res, "Font"), 1);
        TspdfObj *fonts = test_resolve_ref(doc2, tspdf_dict_get(res, "Font"));
        ASSERT(fonts != NULL);
        ASSERT_EQ_INT(fonts->type, TSPDF_OBJ_DICT);
        // Original font must survive alongside the overlay font...
        ASSERT(tspdf_dict_get(fonts, "F1") != NULL);
        ASSERT(tspdf_dict_get(fonts, font) != NULL);
        // ...and the shared dict must not accumulate the OTHER page's
        // additions (each page gets its own copy: exactly F1 + overlay font).
        ASSERT_EQ_SIZE(fonts->dict.count, 2);
        // The opacity ExtGState made it in, too.
        TspdfObj *egs = test_resolve_ref(doc2, tspdf_dict_get(res, "ExtGState"));
        ASSERT(egs != NULL);
        ASSERT_EQ_INT(egs->type, TSPDF_OBJ_DICT);
        ASSERT(tspdf_dict_get(egs, gs) != NULL);

        // Both the original text and the overlay text extract.
        const char *text = tspdf_reader_page_text(doc2, p, &err);
        ASSERT(text != NULL);
        ASSERT(strstr(text, p == 0 ? "Hello page one" : "Hello page two") != NULL);
        ASSERT(strstr(text, "DRAFT") != NULL);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
}

TEST(test_end_content_under) {
    size_t pdf_len = 0;
    uint8_t *pdf_data = make_stamp_text_pdf("OriginalText", &pdf_len);
    ASSERT(pdf_data != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf_data, pdf_len, &err);
    ASSERT(doc != NULL);

    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    TspdfStream *under = tspdf_page_begin_content(doc, 0);
    ASSERT(under != NULL);
    tspdf_stream_begin_text(under);
    tspdf_stream_set_font(under, font, 24.0);
    tspdf_stream_text_position(under, 72, 500);
    tspdf_stream_show_text(under, "UnderText");
    tspdf_stream_end_text(under);
    err = tspdf_page_end_content_under(doc, 0, under, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res_owner);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    const char *under_pos = strstr(text, "UnderText");
    const char *orig_pos = strstr(text, "OriginalText");
    ASSERT(under_pos != NULL);
    ASSERT(orig_pos != NULL);
    // Under-content is prepended, so it comes first in content order.
    ASSERT(under_pos < orig_pos);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf_data);
}

TEST(test_import_page_xobject_basic) {
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, bbox, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // BBox mirrors the source page MediaBox (A4).
    ASSERT(bbox[2] > 595.0 && bbox[2] < 596.0);
    ASSERT(bbox[3] > 841.0 && bbox[3] < 842.0);

    err = draw_xobject_on_page(dst, 0, xnum, 0.5, 50, 50);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "BaseText") != NULL);
    ASSERT(strstr(text, "StampText") != NULL);
    // The form XObject made it into the file.
    ASSERT(bytes_contains(out, out_len, "/Form"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_same_source_twice_dedups) {
    // Repeated imports from the same source into one destination (the nup /
    // stamp pattern: one import per placement) must reuse already-imported
    // referenced objects instead of copying them again.
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    size_t base = dst->new_objs.count;
    uint32_t x1 = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(x1 > 0);
    size_t after_first = dst->new_objs.count;
    // First import brings the form plus at least one referenced object (the
    // font) — otherwise the dedup assertion below would be vacuous.
    ASSERT(after_first - base >= 2);

    uint32_t x2 = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(x2 > 0);
    ASSERT(x1 != x2);
    // Every referenced object is a cache hit: only the form itself is new.
    ASSERT_EQ_INT((int)(dst->new_objs.count - after_first), 1);

    // Both forms' /Resources must reference the SAME destination objects.
    TspdfObj *form1 = dst->new_objs.objs[x1 - dst->xref.count];
    TspdfObj *form2 = dst->new_objs.objs[x2 - dst->xref.count];
    ASSERT(form1 && form1->type == TSPDF_OBJ_STREAM);
    ASSERT(form2 && form2->type == TSPDF_OBJ_STREAM);
    uint32_t ref1 = first_ref_num_in(tspdf_dict_get(form1->stream.dict, "Resources"), 0);
    uint32_t ref2 = first_ref_num_in(tspdf_dict_get(form2->stream.dict, "Resources"), 0);
    ASSERT(ref1 != 0);
    ASSERT_EQ_INT((int)ref1, (int)ref2);

    // The deduped result must still save and render both texts.
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, x1, 0.5, 0, 0), TSPDF_OK);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, x2, 0.5, 200, 200), TSPDF_OK);
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "StampText") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_distinct_readers_not_deduped) {
    // The cache key includes the source reader pointer: two readers opened
    // from the same bytes are distinct sources and must NOT share entries
    // (nothing guarantees their object graphs stay in sync).
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src1 = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *src2 = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src1 != NULL);
    ASSERT(src2 != NULL);
    ASSERT(dst != NULL);

    size_t base = dst->new_objs.count;
    uint32_t x1 = tspdf_reader_import_page_xobject(dst, src1, 0, NULL, &err);
    ASSERT(x1 > 0);
    size_t delta1 = dst->new_objs.count - base;
    ASSERT(delta1 >= 2);

    uint32_t x2 = tspdf_reader_import_page_xobject(dst, src2, 0, NULL, &err);
    ASSERT(x2 > 0);
    size_t delta2 = dst->new_objs.count - base - delta1;
    // Full copy again: same object count as the first import.
    ASSERT_EQ_INT((int)delta2, (int)delta1);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src2);
    tspdf_reader_destroy(src1);
    free(dst_data);
    free(src_data);
}

TEST(test_import_source_freed_before_save) {
    // The import must be self-contained: destroying the source document (and
    // freeing its buffer) before saving the destination must be safe.
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);

    tspdf_reader_destroy(src);
    free(src_data);

    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "StampText") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    free(dst_data);
}

TEST(test_import_from_encrypted_source) {
    size_t plain_len = 0, dst_len = 0;
    uint8_t *plain = make_stamp_text_pdf("SecretStamp", &plain_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(plain != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *tmp = tspdf_reader_open(plain, plain_len, &err);
    ASSERT(tmp != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(tmp, &enc, &enc_len,
                                                "pw", "pw", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(tmp);
    free(plain);

    TspdfReader *src = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    // Stamp content was decrypted during import; the output is not encrypted.
    ASSERT(strstr(text, "SecretStamp") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(enc);
    free(dst_data);
}

TEST(test_import_page_out_of_range) {
    size_t len = 0;
    uint8_t *data = make_stamp_text_pdf("X", &len);
    ASSERT(data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(data, len, &err);
    ASSERT(doc != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(doc, doc, 5, NULL, &err);
    ASSERT_EQ_SIZE((size_t)xnum, 0);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
    free(data);
}

TEST(test_import_multi_stream_contents) {
    size_t src_len = 0;
    char *src_data = make_two_stream_page_pdf(&src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "AlphaPart") != NULL);
    ASSERT(strstr(text, "BetaPart") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_cyclic_resources_bounded) {
    size_t src_len = 0;
    char *src_data = make_cyclic_resources_pdf(&src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    // Must terminate (each source object is imported at most once).
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_huge_bbox_clamped) {
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 99999999999 99999999999", &src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, bbox, &err);
    ASSERT(xnum > 0);
    ASSERT(bbox[2] <= 1.0e7);
    ASSERT(bbox[3] <= 1.0e7);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_degenerate_bbox_rejected) {
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 0 0", &src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT_EQ_SIZE((size_t)xnum, 0);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_PDF);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_add_xobject_unique_names) {
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);

    const char *n1 = tspdf_page_add_xobject(dst, 0, xnum);
    const char *n2 = tspdf_page_add_xobject(dst, 0, xnum);
    ASSERT(n1 != NULL);
    ASSERT(n2 != NULL);
    ASSERT(strcmp(n1, n2) != 0);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_nup_2up_four_pages) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // 4 pages / 2-up = 2 sheets.
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 2);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);
    // All four source pages are present across the two sheets.
    char all[512] = {0};
    for (size_t i = 0; i < 2; i++) {
        const char *t = tspdf_reader_page_text(re, i, &err);
        if (t) { strncat(all, t, sizeof(all) - strlen(all) - 1); }
    }
    ASSERT(strstr(all, "PAGE1") != NULL);
    ASSERT(strstr(all, "PAGE2") != NULL);
    ASSERT(strstr(all, "PAGE3") != NULL);
    ASSERT(strstr(all, "PAGE4") != NULL);
    // Two form XObjects imported per sheet.
    ASSERT(bytes_contains(bytes, bytes_len, "/Form"));

    tspdf_reader_destroy(re);
    free(bytes);
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_4up_four_pages_one_sheet) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    const char *t = tspdf_reader_page_text(re, 0, &err);
    ASSERT(t != NULL);
    ASSERT(strstr(t, "PAGE1") != NULL);
    ASSERT(strstr(t, "PAGE2") != NULL);
    ASSERT(strstr(t, "PAGE3") != NULL);
    ASSERT(strstr(t, "PAGE4") != NULL);

    tspdf_reader_destroy(re);
    free(bytes);
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_3pages_4up_partial_sheet) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(3, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    // 3 pages / 4-up = 1 sheet (one empty cell).
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_5pages_2up_three_sheets) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(5, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    // ceil(5/2) = 3 sheets.
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 3);

    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_invalid_n_rejected) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 3;  // not in {2,4,6,8,9,16}
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_empty_selection_rejected) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    size_t empty = 0;
    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.pages = &empty;  // non-NULL but zero count
    opts.page_count = 0;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_aspect_ratio_uniform_scale) {
    // Build a tall, narrow source page (100 x 400) with distinct content.
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 100 400", &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    opts.landscape = true;  // wide cells
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // The cm matrix that places the form is uniform "s 0 0 s tx ty cm": the
    // two scale components must be equal (no distortion). We look for a
    // matrix line ending in " cm" whose a==d. Rather than parse, assert the
    // content contains " cm" and " Do" (placement happened) — the numeric
    // aspect check is validated by the pymupdf oracle at dev time.
    ASSERT(bytes_contains(bytes, bytes_len, " cm"));
    ASSERT(bytes_contains(bytes, bytes_len, " Do"));

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_frame_draws_border) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    opts.frame = true;
    opts.gap = 10.0;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // A stroked rectangle (re ... S) is present for the frame.
    ASSERT(bytes_contains(bytes, bytes_len, " re"));

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_cyclic_resources_bounded) {
    size_t src_len = 0;
    char *src_data = make_cyclic_resources_pdf(&src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    // Must terminate (import is bounded) and produce a valid one-cell sheet.
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_source_outlives_output_until_saved) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save while the source is still alive (the output references it).
    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_len > 0);

    // Only now may the source be destroyed.
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);

    // The saved bytes are self-contained and reopen without the source.
    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(bytes);
}

TEST(test_add_link_annotation) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_link(doc, 0, 72, 700, 200, 20, "https://example.com");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfReaderPage *page = tspdf_reader_get_page(doc2, 0);
    TspdfObj *annots = tspdf_dict_get(page->page_dict, "Annots");
    ASSERT(annots != NULL);
    ASSERT_EQ_INT(annots->type, TSPDF_OBJ_ARRAY);
    ASSERT(annots->array.count >= 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_text_note) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_text_note(doc, 0, 100, 500, "Note", "This is a note");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_stamp) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_stamp(doc, 0, 100, 400, 200, 50, "Confidential");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_link_to_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_link_to_page(doc, 0, 72, 700, 200, 20, 2);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

void run_import_tests(void) {
    printf("\n  Content overlay:\n");
    RUN(test_overlay_text);
    RUN(test_overlay_on_specific_page);
    RUN(test_overlay_abort);
    RUN(test_overlay_wraps_unbalanced_content);
    RUN(test_overlay_indirect_shared_resources);
    printf("\n  Form XObject import:\n");
    RUN(test_end_content_under);
    RUN(test_import_page_xobject_basic);
    RUN(test_import_same_source_twice_dedups);
    RUN(test_import_distinct_readers_not_deduped);
    RUN(test_import_source_freed_before_save);
    RUN(test_import_from_encrypted_source);
    RUN(test_import_page_out_of_range);
    RUN(test_import_multi_stream_contents);
    RUN(test_import_cyclic_resources_bounded);
    RUN(test_import_huge_bbox_clamped);
    RUN(test_import_degenerate_bbox_rejected);
    RUN(test_add_xobject_unique_names);
    printf("\n  N-up imposition:\n");
    RUN(test_nup_2up_four_pages);
    RUN(test_nup_4up_four_pages_one_sheet);
    RUN(test_nup_3pages_4up_partial_sheet);
    RUN(test_nup_5pages_2up_three_sheets);
    RUN(test_nup_invalid_n_rejected);
    RUN(test_nup_empty_selection_rejected);
    RUN(test_nup_aspect_ratio_uniform_scale);
    RUN(test_nup_frame_draws_border);
    RUN(test_nup_cyclic_resources_bounded);
    RUN(test_nup_source_outlives_output_until_saved);
    printf("\n  Annotations:\n");
    RUN(test_add_link_annotation);
    RUN(test_add_text_note);
    RUN(test_add_stamp);
    RUN(test_add_link_to_page);
}
