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
#include "../../src/reader/tspr_text.h"

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

static uint8_t *dt_writer_pdf(int npages, bool outlines, bool fields,
                              const char *tag, const char *font_name,
                              size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, font_name);
    for (int i = 0; i < npages; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 14);
        tspdf_stream_move_to(page, 72, 700);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s page %d", tag, i + 1);
        tspdf_stream_show_text(page, buf);
        tspdf_stream_end_text(page);
    }
    if (outlines) {
        char t[128];
        snprintf(t, sizeof(t), "%s-CH1", tag);
        int ch1 = tspdf_writer_add_bookmark(w, t, 0);
        snprintf(t, sizeof(t), "%s-CH1-SUB", tag);
        tspdf_writer_add_child_bookmark(w, ch1, t, npages > 1 ? 1 : 0);
        snprintf(t, sizeof(t), "%s-CH2", tag);
        tspdf_writer_add_bookmark(w, t, npages - 1);
    }
    if (fields) {
        char n[128];
        snprintf(n, sizeof(n), "%s_text", tag);
        tspdf_writer_add_text_field(w, 0, n, 72, 600, 200, 20, "hello",
                                    font_name, 12);
        snprintf(n, sizeof(n), "%s_check", tag);
        tspdf_writer_add_checkbox(w, npages - 1, n, 72, 560, 14, true);
    }
    uint8_t *data = NULL;
    TspdfError err = tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    if (err != TSPDF_OK) {
        free(data);
        return NULL;
    }
    return data;
}

static uint8_t *wasm_read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return NULL; }
    *out_len = nread;
    return buf;
}

static const TspdfFormFieldInfo *form_find(TspdfFormFieldInfo *fields,
                                           size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (fields[i].name && strcmp(fields[i].name, name) == 0) {
            return &fields[i];
        }
    }
    return NULL;
}

static bool form_has_option(const TspdfFormFieldInfo *f, const char *opt) {
    for (size_t i = 0; i < f->option_count; i++) {
        if (f->options[i] && strcmp(f->options[i], opt) == 0) return true;
    }
    return false;
}

static char *form_make_cyclic_kids_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (loop) /Kids [3 0 R 5 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /T (kid) /Kids [3 0 R] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *form_make_self_kid_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (self) /Kids [3 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *form_make_adversarial_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R 5 0 R 6 0 R 88 0 R 42] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /Type /Annot /Subtype /Widget >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [97 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /FT /Tx /T (dangler) /Kids [98 0 R] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /FT /Btn /T (chk) /Type /Annot /Subtype /Widget "
                 "/Rect [10 10 20 20] "
                 "/AP << /N << /On 96 0 R /Off 95 0 R >> >> >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static TspdfReader *form_reopen(TspdfReader *doc, uint8_t **out_buf) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (tspdf_reader_save_to_memory(doc, &out, &out_len) != TSPDF_OK) {
        free(out);
        return NULL;
    }
    TspdfError err;
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    if (!re) {
        free(out);
        return NULL;
    }
    *out_buf = out;
    return re;
}

static char *form_make_inherited_opt_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R 6 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Ch /T (size) /Opt [(S) (M) (L)] "
                 "/Kids [5 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [5 0 R 7 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /T (inner) /Type /Annot /Subtype /Widget "
                 "/Rect [10 700 110 720] /Parent 3 0 R >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /FT /Ch /T (pair) "
                 "/Opt [[(ex1) (Display One)] [(ex2) (Display Two)]] "
                 "/Kids [7 0 R] >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /T (k) /Type /Annot /Subtype /Widget "
                 "/Rect [10 660 110 680] /Parent 6 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 8\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 7; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

#define FALLBACK_TTF "tests/data/fallback_font.ttf"

#define FALLBACK_TTC "tests/data/fallback_font.ttc"

#define FALLBACK_LATIN_TTF "tests/data/fallback_latin.ttf"

static void fallback_env(const char *font, const char *dirs) {
    if (font) setenv("TSPDF_FALLBACK_FONT", font, 1);
    else unsetenv("TSPDF_FALLBACK_FONT");
    if (dirs) setenv("TSPDF_FONT_DIRS", dirs, 1);
    else unsetenv("TSPDF_FONT_DIRS");
}

static char *form_make_readonly_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [4 0 R] /DA (/Helv 12 Tf 0 g) >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [4 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /FT /Tx /T (locked) /Ff 1 /Type /Annot /Subtype /Widget "
                 "/Rect [72 700 300 720] /P 3 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *form_make_evil_da_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (evil) /Type /Annot /Subtype /Widget "
                 "/Rect [10 10 200 30] /DA (/Bad\\(Font\\)Name 12 Tf) "
                 "/P 4 0 R >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [3 0 R] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static TspdfObj *flat_resolve(TspdfReader *doc, TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_REF) return obj;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    if (obj->ref.num < doc->xref.count) {
        return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num,
                                  doc->obj_cache, doc->crypt);
    }
    return NULL;
}

static size_t flat_page_font_count(TspdfReader *doc, size_t page) {
    TspdfObj *pd = tspdf_reader_get_page(doc, page)->page_dict;
    TspdfObj *res = flat_resolve(doc, tspdf_dict_get(pd, "Resources"));
    if (!res || res->type != TSPDF_OBJ_DICT) return 0;
    TspdfObj *font = flat_resolve(doc, tspdf_dict_get(res, "Font"));
    if (!font || font->type != TSPDF_OBJ_DICT) return 0;
    return font->dict.count;
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

static TspdfReader *t2_form_fill_and_save(const char *value,
                                          uint8_t **out, size_t *out_len) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    if (!doc) return NULL;
    err = tspdf_reader_form_fill(doc, "name", value, false);
    if (err != TSPDF_OK) { tspdf_reader_destroy(doc); return NULL; }

    *out = NULL; *out_len = 0;
    err = tspdf_reader_save_to_memory(doc, out, out_len);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) { free(*out); return NULL; }

    TspdfReader *re = tspdf_reader_open(*out, *out_len, &err);
    if (!re) { free(*out); return NULL; }
    return re;
}

TEST(test_form_fields_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 5);

    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT_EQ_INT(name->type, TSPDF_FIELD_TEXT);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);
    ASSERT(!name->readonly);
    ASSERT(!name->required);
    ASSERT_EQ_SIZE(name->page_index, 0);
    ASSERT(name->rect[0] == 72 && name->rect[1] == 672 &&
           name->rect[2] == 300 && name->rect[3] == 692);

    const TspdfFormFieldInfo *agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT_EQ_INT(agree->type, TSPDF_FIELD_CHECKBOX);
    ASSERT(agree->value && strcmp(agree->value, "Off") == 0);
    ASSERT_EQ_SIZE(agree->option_count, 1);
    ASSERT(form_has_option(agree, "Yes"));

    const TspdfFormFieldInfo *city = form_find(fields, count, "city");
    ASSERT(city != NULL);
    ASSERT_EQ_INT(city->type, TSPDF_FIELD_CHOICE);
    ASSERT(city->value && strcmp(city->value, "Berlin") == 0);
    ASSERT_EQ_SIZE(city->option_count, 3);
    ASSERT(form_has_option(city, "Berlin"));
    ASSERT(form_has_option(city, "Paris"));
    ASSERT(form_has_option(city, "Oslo"));

    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT_EQ_INT(color->type, TSPDF_FIELD_RADIO);
    ASSERT(color->value && strcmp(color->value, "Red") == 0);
    ASSERT_EQ_SIZE(color->option_count, 2);
    ASSERT(form_has_option(color, "Red"));
    ASSERT(form_has_option(color, "Blue"));
    ASSERT_EQ_SIZE(color->page_index, 0);

    // Hierarchical field: parent /T (a), widget kid /T (b).
    const TspdfFormFieldInfo *ab = form_find(fields, count, "a.b");
    ASSERT(ab != NULL);
    ASSERT_EQ_INT(ab->type, TSPDF_FIELD_TEXT);
    ASSERT(ab->value == NULL);
    ASSERT_EQ_SIZE(ab->page_index, 0);

    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_no_form) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 123;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 0);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_encrypted) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 5);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);
    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT(color->value && strcmp(color->value, "Red") == 0);

    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_writer_roundtrip) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(2, false, true, "W1", "Helvetica", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 2);

    const TspdfFormFieldInfo *text = form_find(fields, count, "W1_text");
    ASSERT(text != NULL);
    ASSERT_EQ_INT(text->type, TSPDF_FIELD_TEXT);
    ASSERT(text->value && strcmp(text->value, "hello") == 0);
    ASSERT_EQ_SIZE(text->page_index, 0);

    // tspdf's own writer emits checkbox /V as a string and no /AP.
    const TspdfFormFieldInfo *check = form_find(fields, count, "W1_check");
    ASSERT(check != NULL);
    ASSERT_EQ_INT(check->type, TSPDF_FIELD_CHECKBOX);
    ASSERT(check->value && strcmp(check->value, "Yes") == 0);
    ASSERT_EQ_SIZE(check->page_index, 1);
    ASSERT_EQ_SIZE(check->option_count, 0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fields_self_kid_listed_once) {
    size_t len = 0;
    char *pdf = form_make_self_kid_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The self-referential field is terminal; it lists exactly once as "self",
    // not stacked "self.self..." names.
    ASSERT_EQ_SIZE(count, 1);
    ASSERT(form_find(fields, count, "self") != NULL);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fields_cyclic_kids_bounded) {
    size_t len = 0;
    char *pdf = form_make_cyclic_kids_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Bounded: the cycle must not fan out into an unbounded field list.
    ASSERT(count < 16);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fields_adversarial_no_crash) {
    size_t len = 0;
    char *pdf = form_make_adversarial_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 3);

    // Unnamed widget field: listed with "" name, no page, zero rect.
    const TspdfFormFieldInfo *unnamed = form_find(fields, count, "");
    ASSERT(unnamed != NULL);
    ASSERT_EQ_INT(unnamed->type, TSPDF_FIELD_TEXT);
    ASSERT(unnamed->page_index == (size_t)-1);
    ASSERT(unnamed->rect[0] == 0 && unnamed->rect[2] == 0);

    // Field whose kid ref dangles still enumerates by name.
    ASSERT(form_find(fields, count, "dangler") != NULL);

    // Button options come from the /AP /N keys even when the streams dangle.
    const TspdfFormFieldInfo *chk = form_find(fields, count, "chk");
    ASSERT(chk != NULL);
    ASSERT_EQ_INT(chk->type, TSPDF_FIELD_CHECKBOX);
    ASSERT_EQ_SIZE(chk->option_count, 1);
    ASSERT(form_has_option(chk, "On"));

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fill_text_value_and_appearance) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Grace Hopper", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_fill(doc, "a.b", "nested", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The appearance stream shows the value (the /V string alone would not
    // produce a "... Tj" operator sequence) and the AcroForm is marked
    // NeedAppearances as a belt for viewers that regenerate.
    ASSERT(bytes_contains(out, out_len, "(Grace Hopper) Tj"));
    ASSERT(bytes_contains(out, out_len, "/NeedAppearances true"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Grace Hopper") == 0);
    const TspdfFormFieldInfo *ab = form_find(fields, count, "a.b");
    ASSERT(ab != NULL);
    ASSERT(ab->value && strcmp(ab->value, "nested") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_text_nonascii_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Gr\xc3\xbc\xc3\x9f" "e", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Gr\xc3\xbc\xc3\x9f" "e") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_checkbox_states) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // Not an on-state of this checkbox.
    err = tspdf_reader_form_fill(doc, "agree", "Bogus", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    err = tspdf_reader_form_fill(doc, "agree", "Yes", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT(agree->value && strcmp(agree->value, "Yes") == 0);

    // And back off.
    err = tspdf_reader_form_fill(re, "agree", "Off", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    uint8_t *out2 = NULL;
    TspdfReader *re2 = form_reopen(re, &out2);
    ASSERT(re2 != NULL);
    err = tspdf_reader_form_fields(re2, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT(agree->value && strcmp(agree->value, "Off") == 0);

    tspdf_reader_destroy(re2);
    free(out2);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_radio_sets_widget_as) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "color", "Blue", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT(color->value && strcmp(color->value, "Blue") == 0);

    // Each widget's /AS follows its own /AP /N states: the "Red" kid goes
    // /Off, the "Blue" kid goes /Blue.
    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL);
    TspdfObj *flds = dt_get(re, acro, "Fields");
    ASSERT(flds && flds->type == TSPDF_OBJ_ARRAY);
    TspdfObj *radio = NULL;
    for (size_t i = 0; i < flds->array.count; i++) {
        TspdfObj *f = test_resolve_ref(re, &flds->array.items[i]);
        if (f && f->type == TSPDF_OBJ_DICT && dt_str_eq(tspdf_dict_get(f, "T"), "color")) {
            radio = f;
            break;
        }
    }
    ASSERT(radio != NULL);
    TspdfObj *kids = dt_get(re, radio, "Kids");
    ASSERT(kids && kids->type == TSPDF_OBJ_ARRAY && kids->array.count == 2);
    int blue_as = 0, off_as = 0;
    for (size_t i = 0; i < kids->array.count; i++) {
        TspdfObj *kid = test_resolve_ref(re, &kids->array.items[i]);
        ASSERT(kid && kid->type == TSPDF_OBJ_DICT);
        TspdfObj *as = tspdf_dict_get(kid, "AS");
        ASSERT(as != NULL);
        if (dt_str_eq(as, "Blue")) blue_as++;
        if (dt_str_eq(as, "Off")) off_as++;
    }
    ASSERT_EQ_INT(blue_as, 1);
    ASSERT_EQ_INT(off_as, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_choice) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "city", "Oslo", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *city = form_find(fields, count, "city");
    ASSERT(city != NULL);
    ASSERT(city->value && strcmp(city->value, "Oslo") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_choice_rejects_non_option) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // "city" is a plain combo (/Ff Combo only) with /Opt Berlin/Paris/Oslo.
    err = tspdf_reader_form_fill(doc, "city", "Atlantis", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    // A listed option and the empty string (clear the value) still work.
    err = tspdf_reader_form_fill(doc, "city", "Paris", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_fill(doc, "city", "", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_editable_combo_accepts_free_text) {
    TspdfError err;
    TspdfReader *doc =
        tspdf_reader_open_file("tests/data/form_combo_edit.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "nickname", "Zaphod", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *nick = form_find(fields, count, "nickname");
    ASSERT(nick != NULL);
    ASSERT(nick->value && strcmp(nick->value, "Zaphod") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_choice_opt_inherited_lists_options) {
    size_t len = 0;
    char *pdf = form_make_inherited_opt_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 2);

    const TspdfFormFieldInfo *inner = form_find(fields, count, "size.inner");
    ASSERT(inner != NULL);
    ASSERT_EQ_INT(inner->type, TSPDF_FIELD_CHOICE);
    ASSERT_EQ_SIZE(inner->option_count, 3);
    ASSERT(form_has_option(inner, "S"));
    ASSERT(form_has_option(inner, "M"));
    ASSERT(form_has_option(inner, "L"));

    // [export display] pairs list the export value, from the parent /Opt.
    const TspdfFormFieldInfo *k = form_find(fields, count, "pair.k");
    ASSERT(k != NULL);
    ASSERT_EQ_SIZE(k->option_count, 2);
    ASSERT(form_has_option(k, "ex1"));
    ASSERT(form_has_option(k, "ex2"));

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_choice_opt_inherited_validates_fill) {
    size_t len = 0;
    char *pdf = form_make_inherited_opt_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // A value outside the parent-held /Opt must be rejected (before the
    // inheritance fix this silently passed because only the terminal dict
    // was consulted).
    err = tspdf_reader_form_fill(doc, "size.inner", "XL", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    // Listed options and clearing still work.
    err = tspdf_reader_form_fill(doc, "size.inner", "M", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_fill(doc, "size.inner", "", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Pairs validate against the export value, not the display string.
    err = tspdf_reader_form_fill(doc, "pair.k", "Display One", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    err = tspdf_reader_form_fill(doc, "pair.k", "ex2", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_fallback_font_score_ordering) {
    // CJK-hinted names come first when CJK coverage is needed...
    ASSERT(tspdf_fallback_font_score("NotoSansCJK-Regular.ttc", true) >
           tspdf_fallback_font_score("DejaVuSans.ttf", true));
    // ...and drop behind general-purpose fonts when it is not.
    ASSERT(tspdf_fallback_font_score("DejaVuSans.ttf", false) >
           tspdf_fallback_font_score("NotoSansCJK-Regular.ttc", false));
    // Regular beats italic/bold variants of the same family.
    ASSERT(tspdf_fallback_font_score("DejaVuSans.ttf", false) >
           tspdf_fallback_font_score("DejaVuSans-BoldItalic.ttf", false));
    // .otf (usually CFF) is parsed last.
    ASSERT(tspdf_fallback_font_score("NotoSans-Regular.ttf", false) >
           tspdf_fallback_font_score("NotoSans-Regular.otf", false));
}

TEST(test_fallback_font_coverage_check) {
    const uint32_t cjk[] = {0x65E5, 0x672C, 0x8A9E};  // 日本語
    const uint32_t latin[] = {'A', 'z', '9'};
    ASSERT(tspdf_fallback_font_covers(FALLBACK_TTF, cjk, 3));
    ASSERT(tspdf_fallback_font_covers(FALLBACK_TTF, latin, 3));
    // The Latin-only sibling parses fine but must fail CJK coverage.
    ASSERT(tspdf_fallback_font_covers(FALLBACK_LATIN_TTF, latin, 3));
    ASSERT(!tspdf_fallback_font_covers(FALLBACK_LATIN_TTF, cjk, 3));
    // Not a font at all.
    ASSERT(!tspdf_fallback_font_covers("tests/data/form_fields.pdf", cjk, 3));
}

TEST(test_fallback_font_ttc_loads) {
    // The .ttc wrapper parses through the ttcf header and covers the same
    // glyphs as the plain .ttf face.
    const uint32_t cjk[] = {0x65E5, 0x30C8};  // 日 ト
    ASSERT(tspdf_fallback_font_covers(FALLBACK_TTC, cjk, 2));

    TTF_Font font;
    ASSERT(ttf_load(&font, FALLBACK_TTC));
    ASSERT(font.glyf_offset != 0 && font.loca_offset != 0);
    ASSERT(ttf_get_glyph_index(&font, 0x65E5) != 0);
    ttf_free(&font);
}

TEST(test_fallback_font_ttc_skips_non_truetype_face) {
    // Synthetic collection: face 0 is CFF-flavored ('OTTO'), face 1 is the
    // real TrueType face. The loader must skip to face 1.
    size_t ttf_len = 0;
    uint8_t *ttf = wasm_read_whole_file(FALLBACK_TTF, &ttf_len);
    ASSERT(ttf != NULL);
    size_t base = 24;
    size_t total = base + ttf_len;
    uint8_t *ttc = (uint8_t *)calloc(1, total);
    ASSERT(ttc != NULL);
    memcpy(ttc, "ttcf", 4);
    ttc[5] = 1;                        // version 1.0
    ttc[11] = 2;                       // numFonts = 2
    ttc[15] = 20;                      // face 0 offset -> the 'OTTO' filler
    ttc[19] = (uint8_t)base;           // face 1 offset -> real sfnt
    memcpy(ttc + 20, "OTTO", 4);
    memcpy(ttc + base, ttf, ttf_len);
    // Rebase the copied face's table offsets (they are file-absolute).
    uint16_t num_tables = (uint16_t)((ttc[base + 4] << 8) | ttc[base + 5]);
    for (uint16_t i = 0; i < num_tables; i++) {
        size_t rec = base + 12 + (size_t)i * 16 + 8;
        uint32_t off = ((uint32_t)ttc[rec] << 24) | ((uint32_t)ttc[rec + 1] << 16) |
                       ((uint32_t)ttc[rec + 2] << 8) | (uint32_t)ttc[rec + 3];
        off += (uint32_t)base;
        ttc[rec] = (uint8_t)(off >> 24);
        ttc[rec + 1] = (uint8_t)(off >> 16);
        ttc[rec + 2] = (uint8_t)(off >> 8);
        ttc[rec + 3] = (uint8_t)off;
    }
    free(ttf);

    TTF_Font font;
    ASSERT(ttf_load_from_memory(&font, ttc, total));  // takes ownership
    ASSERT(ttf_get_glyph_index(&font, 0x65E5) != 0);
    ttf_free(&font);
}

TEST(test_form_fill_fallback_embeds_cid_font) {
    fallback_env(FALLBACK_TTF, NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // 日本語テスト
    err = tspdf_reader_form_fill(doc, "name",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
        false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The full CIDFontType2/Identity-H chain is embedded and referenced.
    ASSERT(bytes_contains(out, out_len, "/Identity-H"));
    ASSERT(bytes_contains(out, out_len, "/CIDFontType2"));
    ASSERT(bytes_contains(out, out_len, "/FontFile2"));
    ASSERT(bytes_contains(out, out_len, "/TspdfFb"));
    ASSERT(bytes_contains(out, out_len, "beginbfchar"));
    // The appearance draws a glyph hex string, not "??????".
    ASSERT(bytes_contains(out, out_len, "> Tj"));
    ASSERT(!bytes_contains(out, out_len, "(?\?\?\?\?\?) Tj"));
    // Viewers must keep our /AP (regeneration would lose the glyphs).
    ASSERT(bytes_contains(out, out_len, "/NeedAppearances false"));

    // /V round-trips unchanged.
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value,
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    fallback_env(NULL, NULL);
}

TEST(test_form_fill_fallback_mixed_value_renders_whole_value) {
    fallback_env(FALLBACK_TTF, NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // Mixed Latin + CJK: the whole value is drawn with the fallback font
    // (documented v1 behavior), so no literal-string fragment remains.
    err = tspdf_reader_form_fill(doc, "name", "Tokyo\xe6\x97\xa5\xe6\x9c\xac",
                                 false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Identity-H"));
    ASSERT(!bytes_contains(out, out_len, "(Tokyo"));

    free(out);
    tspdf_reader_destroy(doc);
    fallback_env(NULL, NULL);
}

TEST(test_form_fill_fallback_unavailable_keeps_lossy_path) {
    // Hermetic "no usable font anywhere": empty scan roots, no override.
    fallback_env(NULL, "/nonexistent-tspdf-font-dir");
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    ASSERT(!tspdf_reader_form_value_renderable(doc,
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));

    err = tspdf_reader_form_fill(doc, "name",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Old behavior: '?' substitution in the appearance, /V intact, and the
    // NeedAppearances belt stays on.
    ASSERT(bytes_contains(out, out_len, "(?\?\?) Tj"));
    ASSERT(!bytes_contains(out, out_len, "/Identity-H"));
    ASSERT(bytes_contains(out, out_len, "/NeedAppearances true"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value &&
           strcmp(name->value, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    fallback_env(NULL, NULL);
}

TEST(test_form_fill_fallback_discovery_scan) {
    // No override: the directory scan (rooted at tests/data via
    // TSPDF_FONT_DIRS) must find the covering font and reject the
    // Latin-only sibling by actual cmap coverage.
    fallback_env(NULL, "tests/data");
    const uint32_t cjk[] = {0x65E5, 0x672C};
    char *path = tspdf_fallback_font_find(cjk, 2);
    ASSERT(path != NULL);
    ASSERT(strstr(path, "fallback_font") != NULL);
    ASSERT(strstr(path, "fallback_latin") == NULL);
    free(path);

    // And nothing covers Hangul in the fixture set.
    const uint32_t hangul[] = {0xD55C};
    path = tspdf_fallback_font_find(hangul, 1);
    ASSERT(path == NULL);
    fallback_env(NULL, NULL);
}

TEST(test_form_value_renderable) {
    fallback_env(FALLBACK_TTF, NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    ASSERT(tspdf_reader_form_value_renderable(doc, "plain ascii"));
    ASSERT(tspdf_reader_form_value_renderable(doc, "Gr\xc3\xbc\xc3\x9f" "e"));
    ASSERT(tspdf_reader_form_value_renderable(doc, ""));
    // Covered by the fixture fallback font.
    ASSERT(tspdf_reader_form_value_renderable(doc,
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"));
    // Hangul is not in the fixture font: not renderable even with it set.
    ASSERT(!tspdf_reader_form_value_renderable(doc, "\xed\x95\x9c"));
    // Invalid UTF-8 keeps the lossy path.
    ASSERT(!tspdf_reader_form_value_renderable(doc, "\xff\xfe"));

    tspdf_reader_destroy(doc);
    fallback_env(NULL, NULL);
}

TEST(test_form_flatten_fallback_font) {
    fallback_env(FALLBACK_TTF, NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Reopen so flatten runs on a saved document (like the CLI does).
    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    err = tspdf_reader_form_flatten(re);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *flat = NULL;
    size_t flat_len = 0;
    err = tspdf_reader_save_to_memory(re, &flat, &flat_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The page content draws the glyph string with the embedded font and
    // the form itself is gone.
    ASSERT(bytes_contains(flat, flat_len, "/TspdfFb"));
    ASSERT(bytes_contains(flat, flat_len, "/Identity-H"));
    ASSERT(bytes_contains(flat, flat_len, "> Tj"));
    ASSERT(!bytes_contains(flat, flat_len, "/AcroForm"));
    ASSERT(!bytes_contains(flat, flat_len, "(?\?\?) Tj"));

    free(flat);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    fallback_env(NULL, NULL);
}

TEST(test_form_fill_encrypted_save_preserves_encryption) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Bob", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Without a password the saved bytes must not open...
    TspdfError open_err;
    TspdfReader *plain = tspdf_reader_open(out, out_len, &open_err);
    ASSERT(plain == NULL);
    ASSERT_EQ_INT(open_err, TSPDF_ERR_ENCRYPTED);

    // ...and the original password must, with the new value visible.
    TspdfReader *re =
        tspdf_reader_open_with_password(out, out_len, "secret", &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Bob") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_decrypt_option_writes_plaintext) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_unknown_name_errors) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_form_fill(doc, "no_such_field", "x", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_readonly_requires_force) {
    size_t len = 0;
    char *pdf = form_make_readonly_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "locked", "nope", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_UNSUPPORTED);
    err = tspdf_reader_form_fill(doc, "locked", "forced", true);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *locked = form_find(fields, count, "locked");
    ASSERT(locked != NULL);
    ASSERT(locked->readonly);
    ASSERT(locked->value && strcmp(locked->value, "forced") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fill_text_da_font_name_sanitized) {
    size_t len = 0;
    char *pdf = form_make_evil_da_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "evil", "hi", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The sanitized DA font is BadFontName (delimiters stripped). It must
    // appear both as the content-stream Tf operand and as the appearance
    // /Resources /Font key -- and they must be identical, so a strict renderer
    // resolves the font.
    ASSERT(bytes_contains(out, out_len, "/BadFontName 12.00 Tf"));
    ASSERT(bytes_contains(out, out_len, "/Font << /BadFontName "));

    // No PDF delimiter from the hostile /DA reaches the content Tf token: the
    // serializer-escaped form "/Bad#28Font#29Name" (which would desync from
    // the content name) must never appear.
    ASSERT(!bytes_contains(out, out_len, "/Bad#28Font"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_flatten_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Grace", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The fixture page carries /Resources as an indirect ref; merging the
    // flattening resources must not leave a duplicate /Resources key behind.
    {
        TspdfObj *page_dict = doc->pages.pages[0].page_dict;
        size_t res_keys = 0;
        for (size_t i = 0; i < page_dict->dict.count; i++) {
            if (strcmp(page_dict->dict.entries[i].key, "Resources") == 0) {
                res_keys++;
            }
        }
        ASSERT_EQ_SIZE(res_keys, 1);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The form is gone: no catalog /AcroForm, no widget annotations.
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "/Widget"));
    // The checked radio ("Red") appearance stream was stamped into the page
    // content as a form XObject.
    ASSERT(bytes_contains(out, out_len, "/TspdfFx0 Do"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT(!tspdf_reader_has_acroform(re));

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 123;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 0);

    // All fixture annotations were widgets, so the page has none left.
    TspdfObj *annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    ASSERT(annots == NULL || annots->array.count == 0);

    // The stamped values extract as page text.
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Grace") != NULL);
    ASSERT(strstr(text, "Berlin") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_flatten_no_form_noop) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_flatten_writer_checkbox_fallback) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(1, false, true, "W2", "Helvetica", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "/Widget"));
    // Vector check-mark fallback for the checked box (distinctive line width).
    ASSERT(bytes_contains(out, out_len, "1.5 w"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "hello") != NULL);  // flattened text field value

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_flatten_shared_resources_not_accumulated) {
    TspdfError err;
    TspdfReader *doc =
        tspdf_reader_open_file("tests/data/form_shared_resources.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);

    // Each page: the original /Helv plus its own /TspdfFf == 2 entries. The
    // pre-fix bug leaked the other page's key in too (Helv + TspdfFf +
    // TspdfFf_2 == 3).
    ASSERT_EQ_SIZE(flat_page_font_count(re, 0), 2);
    ASSERT_EQ_SIZE(flat_page_font_count(re, 1), 2);

    // Both field values must still render on their own page.
    const char *t0 = tspdf_reader_page_text(re, 0, &err);
    const char *t1 = tspdf_reader_page_text(re, 1, &err);
    ASSERT(t0 && strstr(t0, "Alpha") != NULL);
    ASSERT(t1 && strstr(t1, "Beta") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_pdftext_form_ascii_stays_raw) {
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_form_fill_and_save("Hello", &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "(Hello)"));
    ASSERT(!bytes_contains(out, out_len, "\\376\\377"));

    TspdfFormFieldInfo *fields = NULL; size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *f = form_find(fields, count, "name");
    ASSERT(f != NULL);
    ASSERT(f->value && strcmp(f->value, "Hello") == 0);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_form_latin1_plus_encodes_utf16be) {
    const char *value = "\xC3\xA9";  /* e-acute */
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_form_fill_and_save(value, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    /* U+00E9 as UTF-16BE: 00 E9; 0x00->\000, 0xE9->\\351 in octal. */
    ASSERT(bytes_contains(out, out_len, "\\000\\351"));

    TspdfFormFieldInfo *fields = NULL; size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *f = form_find(fields, count, "name");
    ASSERT(f != NULL);
    ASSERT(f->value && strcmp(f->value, value) == 0);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_form_cjk_encodes_utf16be) {
    const char *value = "\xE6\xBC\xA2";  /* U+6F22 */
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_form_fill_and_save(value, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    /* Bytes 6F 22 are printable 'o' and '"' -- check full BOM+code-unit. */
    ASSERT(bytes_contains(out, out_len, "\\376\\377o\""));

    TspdfFormFieldInfo *fields = NULL; size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *f = form_find(fields, count, "name");
    ASSERT(f != NULL);
    ASSERT(f->value && strcmp(f->value, value) == 0);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_form_astral_encodes_surrogate_pair) {
    const char *value = "\xF0\x9F\x98\x80";  /* U+1F600 */
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_form_fill_and_save(value, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    /* High surrogate D83D: D8->\\330, 3D->'=' */
    ASSERT(bytes_contains(out, out_len, "\\376\\377\\330="));
    /* Full pattern including low surrogate NUL byte -- use binary search. */
    const uint8_t bom_surr[] = {
        '\\','3','7','6','\\','3','7','7',  /* \376\377 (BOM)           */
        '\\','3','3','0','=',               /* \330=    (D8 3D)         */
        '\\','3','3','6','\\','0','0','0'   /* \336\000 (DE 00)         */
    };
    ASSERT(bytes_contains_bin(out, out_len, bom_surr, sizeof(bom_surr)));

    TspdfFormFieldInfo *fields = NULL; size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *f = form_find(fields, count, "name");
    ASSERT(f != NULL);
    ASSERT(f->value && strcmp(f->value, value) == 0);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_pdftext_form_mixed_roundtrip) {
    const char *value = "A\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80";
    uint8_t *out = NULL; size_t out_len = 0;
    TspdfReader *re = t2_form_fill_and_save(value, &out, &out_len);
    ASSERT(re != NULL);

    ASSERT(bytes_contains(out, out_len, "\\376\\377"));

    TspdfFormFieldInfo *fields = NULL; size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *f = form_find(fields, count, "name");
    ASSERT(f != NULL);
    ASSERT(f->value && strcmp(f->value, value) == 0);

    tspdf_reader_destroy(re);
    free(out);
}

void run_form_tests(void) {
    printf("\n  AcroForm fields (list/fill/flatten):\n");
    RUN(test_form_fields_fixture);
    RUN(test_form_fields_no_form);
    RUN(test_form_fields_encrypted);
    RUN(test_form_fields_writer_roundtrip);
    RUN(test_form_fields_cyclic_kids_bounded);
    RUN(test_form_fields_self_kid_listed_once);
    RUN(test_form_fields_adversarial_no_crash);
    RUN(test_form_fill_text_value_and_appearance);
    RUN(test_form_fill_text_da_font_name_sanitized);
    RUN(test_form_fill_text_nonascii_roundtrip);
    RUN(test_form_fill_checkbox_states);
    RUN(test_form_fill_radio_sets_widget_as);
    RUN(test_form_fill_choice);
    RUN(test_form_fill_choice_rejects_non_option);
    RUN(test_form_fill_editable_combo_accepts_free_text);
    RUN(test_form_choice_opt_inherited_lists_options);
    RUN(test_form_choice_opt_inherited_validates_fill);
    RUN(test_fallback_font_score_ordering);
    RUN(test_fallback_font_coverage_check);
    RUN(test_fallback_font_ttc_loads);
    RUN(test_fallback_font_ttc_skips_non_truetype_face);
    RUN(test_form_fill_fallback_embeds_cid_font);
    RUN(test_form_fill_fallback_mixed_value_renders_whole_value);
    RUN(test_form_fill_fallback_unavailable_keeps_lossy_path);
    RUN(test_form_fill_fallback_discovery_scan);
    RUN(test_form_value_renderable);
    RUN(test_form_flatten_fallback_font);
    RUN(test_form_fill_encrypted_save_preserves_encryption);
    RUN(test_save_decrypt_option_writes_plaintext);
    RUN(test_form_fill_unknown_name_errors);
    RUN(test_form_fill_readonly_requires_force);
    RUN(test_form_flatten_fixture);
    RUN(test_form_flatten_no_form_noop);
    RUN(test_form_flatten_writer_checkbox_fallback);
    RUN(test_form_flatten_shared_resources_not_accumulated);
    printf("\n  Task 2: pdftext pin-down (form text strings):\n");
    RUN(test_pdftext_form_ascii_stays_raw);
    RUN(test_pdftext_form_latin1_plus_encodes_utf16be);
    RUN(test_pdftext_form_cjk_encodes_utf16be);
    RUN(test_pdftext_form_astral_encodes_surrogate_pair);
    RUN(test_pdftext_form_mixed_roundtrip);
}
