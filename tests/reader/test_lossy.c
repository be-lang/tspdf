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

static char *lossy_make_ctm_pdf(size_t *out_len) {
    const size_t cap = 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[10] = {0};

    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources << /XObject << /Im1 4 0 R /Im2 5 0 R /Im3 6 0 R /Fm1 7 0 R >> >> "
                 "/Contents 8 0 R >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Type /XObject /Subtype /Image /Width 800 /Height 600 "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 400 /Height 300 "
                 "/ColorSpace /DeviceGray /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Type /XObject /Subtype /Image /Width 640 /Height 480 "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    // Form XObject drawing Im2 at 400x300 under its own /Matrix [.5 0 0 .5].
    {
        const char *form = "q 400 0 0 300 20 20 cm /Im2 Do Q";
        off[7] = pos;
        if (!appendf(pdf, cap, &pos,
                     "7 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                     "/Matrix [0.5 0 0 0.5 0 0] "
                     "/Resources << /XObject << /Im2 5 0 R >> >> /Length %zu >>\n"
                     "stream\n%s\nendstream\nendobj\n",
                     strlen(form), form)) goto fail;
    }
    // Page content: Im1 small, then (nested q/Q) Im1 large; the form is
    // drawn under an outer 1.5 scale, so Im2 renders at 400*0.5*1.5 = 300 x
    // 300*0.5*1.5 = 225 pt.
    {
        const char *content =
            "q 100 0 0 75 10 10 cm /Im1 Do Q "
            "q 1 0 0 1 5 5 cm q 200 0 0 150 0 0 cm /Im1 Do Q Q "
            "q 1.5 0 0 1.5 0 0 cm /Fm1 Do Q";
        off[8] = pos;
        if (!appendf(pdf, cap, &pos,
                     "8 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                     strlen(content), content)) goto fail;
    }

    {
        size_t xref = pos;
        if (!appendf(pdf, cap, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 8; i++) {
            if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, cap, &pos,
                     "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static const TspdfLossyPlacement *lossy_find_placement(
        const TspdfLossyPlacement *p, size_t n, uint32_t obj_num) {
    for (size_t i = 0; i < n; i++) {
        if (p[i].obj_num == obj_num) return &p[i];
    }
    return NULL;
}

static uint8_t *lossy_make_image_pdf_bpc(const uint8_t *img, size_t img_len,
                                         const char *img_extra, int w, int h,
                                         int bpc, const char *cs, double w_pt,
                                         double h_pt, size_t *out_len) {
    char content[128];
    snprintf(content, sizeof(content), "q %.2f 0 0 %.2f 10 10 cm /Im1 Do Q",
             w_pt, h_pt);

    size_t cap = img_len + 4096;
    uint8_t *pdf = (uint8_t *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    char head[512];

#define LPUT(...) do { \
        int _n = snprintf(head, sizeof(head), __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(head) || pos + (size_t)_n > cap) goto fail; \
        memcpy(pdf + pos, head, (size_t)_n); \
        pos += (size_t)_n; \
    } while (0)

    LPUT("%%PDF-1.5\n%%\xE2\xE3\xCF\xD3\n");
    off[1] = pos;
    LPUT("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = pos;
    LPUT("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = pos;
    LPUT("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");
    off[4] = pos;
    LPUT("4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
         "/ColorSpace /%s /BitsPerComponent %d %s /Length %zu >>\nstream\n",
         w, h, cs, bpc, img_extra ? img_extra : "", img_len);
    if (pos + img_len > cap) goto fail;
    memcpy(pdf + pos, img, img_len);
    pos += img_len;
    LPUT("\nendstream\nendobj\n");
    off[5] = pos;
    LPUT("5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
         strlen(content), content);

    {
        size_t xref = pos;
        LPUT("xref\n0 6\n0000000000 65535 f \n");
        for (int i = 1; i <= 5; i++) LPUT("%010zu 00000 n \n", off[i]);
        LPUT("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref);
    }
#undef LPUT

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static uint8_t *lossy_make_image_pdf(const uint8_t *img, size_t img_len,
                                     const char *img_extra, int w, int h,
                                     const char *cs, double w_pt, double h_pt,
                                     size_t *out_len) {
    return lossy_make_image_pdf_bpc(img, img_len, img_extra, w, h, 8, cs,
                                    w_pt, h_pt, out_len);
}

static TspdfObj *lossy_get_image_obj(TspdfReader *doc) {
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, 4, doc->obj_cache, doc->crypt);
}

static uint8_t *lossy_make_content_image_pdf(const uint8_t *img, size_t img_len,
                                             const char *img_extra, int w, int h,
                                             const char *cs, const char *content,
                                             size_t *out_len) {
    size_t content_len = strlen(content);
    size_t cap = img_len + content_len + 4096;
    uint8_t *pdf = (uint8_t *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    char head[512];

#define LPUT(...) do { \
        int _n = snprintf(head, sizeof(head), __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(head) || pos + (size_t)_n > cap) goto fail; \
        memcpy(pdf + pos, head, (size_t)_n); \
        pos += (size_t)_n; \
    } while (0)

    LPUT("%%PDF-1.5\n%%\xE2\xE3\xCF\xD3\n");
    off[1] = pos;
    LPUT("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = pos;
    LPUT("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = pos;
    LPUT("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");
    off[4] = pos;
    LPUT("4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
         "/ColorSpace /%s /BitsPerComponent 8 %s /Length %zu >>\nstream\n",
         w, h, cs, img_extra ? img_extra : "", img_len);
    if (pos + img_len > cap) goto fail;
    memcpy(pdf + pos, img, img_len);
    pos += img_len;
    LPUT("\nendstream\nendobj\n");
    off[5] = pos;
    LPUT("5 0 obj\n<< /Length %zu >>\nstream\n", content_len);
    if (pos + content_len > cap) goto fail;
    memcpy(pdf + pos, content, content_len);
    pos += content_len;
    LPUT("\nendstream\nendobj\n");

    {
        size_t xref = pos;
        LPUT("xref\n0 6\n0000000000 65535 f \n");
        for (int i = 1; i <= 5; i++) LPUT("%010zu 00000 n \n", off[i]);
        LPUT("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref);
    }
#undef LPUT

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static char *lossy_deepq_content(int nq) {
    size_t cap = 32 + (size_t)nq * 4 + 16;
    char *c = (char *)malloc(cap);
    if (!c) return NULL;
    size_t pos = (size_t)snprintf(c, cap, "q 500 0 0 500 0 0 cm ");
    for (int i = 0; i < nq; i++) { c[pos++] = 'q'; c[pos++] = ' '; }
    for (int i = 0; i < nq; i++) { c[pos++] = 'Q'; c[pos++] = ' '; }
    memcpy(c + pos, "/Im1 Do Q", sizeof("/Im1 Do Q"));
    return c;
}

static uint8_t *lossy_mono_text_pixels(int w, int h) {
    uint8_t *px = (uint8_t *)malloc((size_t)w * h);
    if (!px) return NULL;
    memset(px, 255, (size_t)w * h);
    uint32_t lcg = 12345;
#define MONO_RND() ((lcg = lcg * 1664525u + 1013904223u) >> 16)
    for (int y0 = 6; y0 + 12 < h; y0 += 15) {
        for (int x0 = 6; x0 + 24 < w; x0 += 26) {
            int ww = 14 + (int)(MONO_RND() % 8);
            for (int s = 0; s < 3; s++) {  // three strokes per "word"
                int e0 = x0 + s * (ww / 3);
                int e1 = e0 + 2 + (int)(MONO_RND() % 3);
                for (int yy = y0; yy < y0 + 10; yy++) {
                    e0 += (int)(MONO_RND() % 3) - 1;
                    e1 += (int)(MONO_RND() % 3) - 1;
                    if (e0 < x0) e0 = x0;
                    if (e1 <= e0) e1 = e0 + 1;
                    if (e1 > x0 + ww) e1 = x0 + ww;
                    for (int xx = e0; xx < e1 && xx < w; xx++)
                        px[(size_t)yy * w + xx] = 0;
                }
            }
        }
    }
#undef MONO_RND
    return px;
}

static uint8_t *lossy_mono_pack(const uint8_t *px, int w, int h, size_t *out_len) {
    size_t stride = ((size_t)w + 7) / 8;
    uint8_t *out = (uint8_t *)calloc(stride, (size_t)h);
    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px[(size_t)y * w + x] >= 128)
                out[(size_t)y * stride + x / 8] |= (uint8_t)(0x80 >> (x % 8));
    *out_len = stride * (size_t)h;
    return out;
}

static bool lossy_mono_check_g4(TspdfReader *doc, int exp_w, int exp_h,
                                uint8_t **out_px) {
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    if (!img_obj || img_obj->type != TSPDF_OBJ_STREAM) return false;
    TspdfObj *d = img_obj->stream.dict;
    TspdfObj *filter = tspdf_dict_get(d, "Filter");
    if (!filter || filter->type != TSPDF_OBJ_NAME ||
        strcmp((const char *)filter->string.data, "CCITTFaxDecode") != 0)
        return false;
    TspdfObj *wobj = tspdf_dict_get(d, "Width");
    TspdfObj *hobj = tspdf_dict_get(d, "Height");
    TspdfObj *bobj = tspdf_dict_get(d, "BitsPerComponent");
    if (!wobj || wobj->integer != exp_w) return false;
    if (!hobj || hobj->integer != exp_h) return false;
    if (!bobj || bobj->integer != 1) return false;
    TspdfObj *parms = tspdf_dict_get(d, "DecodeParms");
    if (!parms || parms->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *k = tspdf_dict_get(parms, "K");
    TspdfObj *cols = tspdf_dict_get(parms, "Columns");
    if (!k || k->integer != -1) return false;
    if (!cols || cols->integer != exp_w) return false;
    if (!img_obj->stream.self_contained || !img_obj->stream.data) return false;

    TspdfCcittParams p;
    tspdf_ccitt_params_default(&p);
    p.k = -1;
    p.columns = exp_w;
    p.rows = exp_h;
    TspdfArena a = tspdf_arena_create(1 << 20);
    TspdfCcittBitmap bm;
    bool ok = tspdf_ccitt_decode(img_obj->stream.data, img_obj->stream.len,
                                 &p, &a, &bm) &&
              bm.width == exp_w && bm.height == exp_h;
    if (ok && out_px) {
        *out_px = (uint8_t *)malloc((size_t)exp_w * exp_h);
        if (*out_px) memcpy(*out_px, bm.pixels, (size_t)exp_w * exp_h);
        else ok = false;
    }
    tspdf_arena_destroy(&a);
    return ok;
}

TEST(test_lossy_box_downsample_exact) {
    // 4x2 gray -> 2x1: each dest pixel is the exact mean of a 2x2 block.
    const uint8_t src[8] = { 10, 20, 100, 200,
                             30, 40, 100,   0 };
    uint8_t dst[2] = { 0xAA, 0xAA };
    ASSERT(tspdf_lossy_box_downsample(src, 4, 2, 1, dst, 2, 1));
    ASSERT_EQ_INT(dst[0], 25);  // (10+20+30+40)/4
    ASSERT_EQ_INT(dst[1], 100); // (100+200+100+0)/4

    // 2x2 RGB -> 1x1: per-channel means.
    const uint8_t rgb[12] = { 0,0,255,  100,50,255,
                              200,150,255, 100,200,255 };
    uint8_t d1[3] = {0};
    ASSERT(tspdf_lossy_box_downsample(rgb, 2, 2, 3, d1, 1, 1));
    ASSERT_EQ_INT(d1[0], 100); // (0+100+200+100)/4
    ASSERT_EQ_INT(d1[1], 100); // (0+50+150+200)/4
    ASSERT_EQ_INT(d1[2], 255);

    // Invalid arguments report failure instead of silently doing nothing.
    ASSERT(!tspdf_lossy_box_downsample(NULL, 4, 2, 1, dst, 2, 1));
    ASSERT(!tspdf_lossy_box_downsample(src, 4, 2, 1, dst, 0, 1));
    ASSERT(!tspdf_lossy_box_downsample(src, 4, 2, 0, dst, 2, 1));
}

TEST(test_lossy_box_downsample_fractional_edges) {
    // 3x1 -> 2x1 (ratio 1.5): dest bin 0 covers source [0,1.5) so pixel 1
    // contributes half its weight; correct edge weighting gives
    // dest0 = (0*1 + 90*0.5)/1.5 = 30, dest1 = (90*0.5 + 30*1)/1.5 = 50.
    const uint8_t src[3] = { 0, 90, 30 };
    uint8_t dst[2] = { 0xAA, 0xAA };
    tspdf_lossy_box_downsample(src, 3, 1, 1, dst, 2, 1);
    ASSERT_EQ_INT(dst[0], 30);
    ASSERT_EQ_INT(dst[1], 50);

    // Same ratio vertically: 1x3 -> 1x2.
    uint8_t dst2[2] = { 0xAA, 0xAA };
    tspdf_lossy_box_downsample(src, 1, 3, 1, dst2, 1, 2);
    ASSERT_EQ_INT(dst2[0], 30);
    ASSERT_EQ_INT(dst2[1], 50);

    // A constant image stays constant under any ratio (weights sum to 1).
    uint8_t flat[7 * 5];
    memset(flat, 137, sizeof(flat));
    uint8_t dflat[3 * 2];
    tspdf_lossy_box_downsample(flat, 7, 5, 1, dflat, 3, 2);
    for (size_t i = 0; i < sizeof(dflat); i++) ASSERT_EQ_INT(dflat[i], 137);
}

TEST(test_lossy_gray_detect) {
    // Near-gray pixels (max channel delta 8) pass.
    uint8_t px[4 * 3] = { 100,104,97,  0,8,4,  255,247,250,  30,30,38 };
    ASSERT(tspdf_lossy_rgb_is_gray(px, 4));

    // One colored pixel anywhere fails.
    px[7] = 120; // pixel 2 becomes (255,120,250)
    ASSERT(!tspdf_lossy_rgb_is_gray(px, 4));

    // Luma conversion: pure gray maps to itself.
    const uint8_t g[2 * 3] = { 100,100,100,  0,0,0 };
    uint8_t out[2] = { 0xAA, 0xAA };
    tspdf_lossy_rgb_to_gray(g, 2, out);
    ASSERT_EQ_INT(out[0], 100);
    ASSERT_EQ_INT(out[1], 0);
}

TEST(test_lossy_target_dims) {
    uint32_t dw = 0, dh = 0;
    // 1000x1000 px rendered at 1x1 inch, target 150 dpi: 6.7x too many
    // pixels -> downsample to 150x150.
    ASSERT(tspdf_lossy_target_dims(1000, 1000, 72.0, 72.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);
    ASSERT_EQ_INT((int)dh, 150);

    // 180x180 px at 1x1 inch is only 1.2x the 150-dpi target: within the
    // 1.3x slack, not worth re-encoding.
    ASSERT(!tspdf_lossy_target_dims(180, 180, 72.0, 72.0, 150, &dw, &dh));

    // Just over the slack acts.
    ASSERT(tspdf_lossy_target_dims(200, 200, 72.0, 72.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);

    // Never upsample: image already below target.
    ASSERT(!tspdf_lossy_target_dims(100, 100, 144.0, 144.0, 150, &dw, &dh));

    // Aspect ratio is preserved; the tighter axis drives the scale.
    // 2000x1000 at 2x0.5 inch, 150 dpi: targets 300x75; height needs
    // 1000/75 = 13.3x, width only 6.7x -> scale by the max => 150x75... no:
    // both dims shrink by the same factor s = max(2000/300, 1000/75) = 13.3,
    // giving 150x75 (both at or below target).
    ASSERT(tspdf_lossy_target_dims(2000, 1000, 144.0, 36.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);
    ASSERT_EQ_INT((int)dh, 75);
    ASSERT(dw <= 300 && dh <= 75);
}

TEST(test_lossy_keep_original_rule) {
    // Replace only when >= 10% smaller than the stored original.
    ASSERT(tspdf_lossy_worth_replacing(1000, 900));   // exactly 10% smaller
    ASSERT(!tspdf_lossy_worth_replacing(1000, 901));  // 9.9%: keep original
    ASSERT(tspdf_lossy_worth_replacing(1000, 100));
    ASSERT(!tspdf_lossy_worth_replacing(1000, 2000)); // grew: keep
    ASSERT(!tspdf_lossy_worth_replacing(0, 0));
}

TEST(test_lossy_ctm_scan_placements) {
    size_t len = 0;
    char *pdf = lossy_make_ctm_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    err = tspdf_lossy_scan_placements(doc, &pl, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 2); // Im1 and Im2; Im3 never drawn

    // Im1 (obj 4): the larger of the two placements wins.
    const TspdfLossyPlacement *im1 = lossy_find_placement(pl, n, 4);
    ASSERT(im1 != NULL);
    ASSERT(im1->w_pt > 199.9 && im1->w_pt < 200.1);
    ASSERT(im1->h_pt > 149.9 && im1->h_pt < 150.1);

    // Im2 (obj 5): drawn only inside the form; form /Matrix (0.5) times the
    // outer cm (1.5) times the inner cm (400x300) = 300 x 225 pt.
    const TspdfLossyPlacement *im2 = lossy_find_placement(pl, n, 5);
    ASSERT(im2 != NULL);
    ASSERT(im2->w_pt > 299.9 && im2->w_pt < 300.1);
    ASSERT(im2->h_pt > 224.9 && im2->h_pt < 225.1);

    // Im3 (obj 6): in /Resources but never drawn.
    ASSERT(lossy_find_placement(pl, n, 6) == NULL);

    free(pl);
    tspdf_reader_destroy(doc);
    free(pdf);

    // The target decision follows from the scan: Im1 is 800px over 200pt
    // (288 dpi) -> at 150 dpi it must shrink; at 300 dpi it must not.
    uint32_t dw = 0, dh = 0;
    ASSERT(tspdf_lossy_target_dims(800, 600, 200.0, 150.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 417); // 150 * 200/72 = 416.7
    ASSERT(!tspdf_lossy_target_dims(800, 600, 200.0, 150.0, 300, &dw, &dh));
}

TEST(test_lossy_predictor15_flate_image_recompressed) {
    // 256x256 gray horizontal gradient, PNG predictor 15 (per-row filter
    // byte; row 0 uses filter 0/None, the rest filter 2/Up so the predictor
    // undo actually runs), deflated.
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc(W * H);
    ASSERT(pixels != NULL);
    uint32_t lcg = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            // Gradient plus a little photo-ish noise: without it the
            // predictor+flate stream is so tiny that keeping the original
            // is (correctly) the better deal.
            lcg = lcg * 1664525u + 1013904223u;
            int v = x + (int)((lcg >> 24) & 7) - 3;
            pixels[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }

    // Predictor-encode: filter byte + row bytes (Up: delta to row above).
    uint8_t *pred = (uint8_t *)malloc((size_t)(W + 1) * H);
    ASSERT(pred != NULL);
    for (int y = 0; y < H; y++) {
        uint8_t *row = pred + (size_t)y * (W + 1);
        row[0] = y == 0 ? 0 : 2;
        for (int x = 0; x < W; x++) {
            uint8_t cur = pixels[y * W + x];
            uint8_t up = y == 0 ? 0 : pixels[(y - 1) * W + x];
            row[1 + x] = y == 0 ? cur : (uint8_t)(cur - up);
        }
    }
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pred, (size_t)(W + 1) * H, &comp_len);
    free(pred);
    ASSERT(comp != NULL);

    // Drawn at 1x1 inch => 256 dpi; at target 150 dpi it must shrink to 150.
    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(
        comp, comp_len,
        "/Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 1 "
        "/BitsPerComponent 8 /Columns 256 >>",
        W, H, "DeviceGray", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    err = tspdf_reader_lossy_images(doc, 150, 75, 300, &st);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);
    ASSERT_EQ_SIZE(st.images_skipped, 0);
    ASSERT(st.bytes_after < st.bytes_before);

    // The stream is now a plain DCTDecode JPEG at the target size, with the
    // predictor parms gone.
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL && img_obj->type == TSPDF_OBJ_STREAM);
    TspdfObj *d = img_obj->stream.dict;
    TspdfObj *filter = tspdf_dict_get(d, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "DCTDecode");
    ASSERT(tspdf_dict_get(d, "DecodeParms") == NULL);
    TspdfObj *wobj = tspdf_dict_get(d, "Width");
    TspdfObj *hobj = tspdf_dict_get(d, "Height");
    ASSERT(wobj && wobj->integer == 150);
    ASSERT(hobj && hobj->integer == 150);

    // The JPEG must decode and still show the gradient (predictor undo fed
    // the right pixels into the downsampler).
    ASSERT(img_obj->stream.self_contained && img_obj->stream.data != NULL);
    TspdfArena a = tspdf_arena_create(1 << 20);
    TspdfRawImage decoded = {0};
    ASSERT(tspdf_jpeg_decode(img_obj->stream.data, img_obj->stream.len, &a, &decoded));
    ASSERT_EQ_INT(decoded.width, 150);
    ASSERT_EQ_INT(decoded.height, 150);
    ASSERT_EQ_INT(decoded.components, 1);
    // Expected value at dest column x is the box average of the source
    // gradient: about (x + 0.5) * 256/150 - 0.5. Allow JPEG error.
    for (int x = 10; x < 150; x += 40) {
        double expect = ((double)x + 0.5) * 256.0 / 150.0 - 0.5;
        int got = decoded.pixels[75 * 150 + x];
        ASSERT(got > expect - 10 && got < expect + 10);
    }
    tspdf_arena_destroy(&a);

    // And the saved file round-trips.
    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts), TSPDF_OK);
    ASSERT(out_len < len); // the gradient JPEG is far smaller than the flate
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(out);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(pixels);
}

TEST(test_lossy_rgb_near_gray_converts_to_devicegray) {
    // 256x256 RGB, every pixel within the gray tolerance -> the rewrite
    // must emit a single-channel /DeviceGray JPEG.
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(pixels != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t v = (uint8_t)((x + y) / 2);
            uint8_t *p = pixels + ((size_t)y * W + x) * 3;
            p[0] = v;
            p[1] = (uint8_t)(v <= 251 ? v + 3 : v);
            p[2] = (uint8_t)(v >= 2 ? v - 2 : v);
        }
    }
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H * 3, &comp_len);
    ASSERT(comp != NULL);

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode",
                                        W, H, "DeviceRGB", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);

    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *cs = tspdf_dict_get(img_obj->stream.dict, "ColorSpace");
    ASSERT(cs && cs->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)cs->string.data, "DeviceGray");

    tspdf_reader_destroy(doc);
    free(pdf);
    free(pixels);
}

TEST(test_lossy_skips_low_dpi_smask_and_small) {
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    ASSERT(comp != NULL);

    // (a) Drawn at 2x2 inches => 128 dpi, already below the 150-dpi target:
    // untouched, counted as skipped.
    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode",
                                        W, H, "DeviceGray", 144.0, 144.0, &len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");
    tspdf_reader_destroy(doc);
    free(pdf);

    // (b) High-dpi but carrying an /SMask: untouched.
    pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode /SMask 4 0 R",
                               W, H, "DeviceGray", 36.0, 36.0, &len);
    ASSERT(pdf != NULL);
    doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    memset(&st, 0, sizeof(st));
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(comp);

    // (c) Below the 65536-pixel floor (255x255): untouched even at high dpi.
    {
        enum { SW = 255, SH = 255 };
        uint8_t *small = (uint8_t *)malloc((size_t)SW * SH);
        ASSERT(small != NULL);
        memset(small, 99, (size_t)SW * SH);
        size_t sc_len = 0;
        uint8_t *sc = deflate_compress(small, (size_t)SW * SH, &sc_len);
        ASSERT(sc != NULL);
        pdf = lossy_make_image_pdf(sc, sc_len, "/Filter /FlateDecode",
                                   SW, SH, "DeviceGray", 36.0, 36.0, &len);
        free(sc);
        ASSERT(pdf != NULL);
        doc = tspdf_reader_open(pdf, len, &err);
        ASSERT(doc != NULL);
        memset(&st, 0, sizeof(st));
        ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
        ASSERT_EQ_SIZE(st.images_recompressed, 0);
        tspdf_reader_destroy(doc);
        free(pdf);
        free(small);
    }
    free(pixels);
}

TEST(test_lossy_deep_q_nesting_measured_at_outer_ctm) {
    // Reviewer repro: q <big cm>, 100 nested q's, 100 Q's, then Do. The Do
    // still happens under the 500x500 cm; a bounded gstate stack that drops
    // saves but pops on Q restores the identity CTM too early and measures
    // the image at 1x1 pt, shrinking a real 512x512 image to nearly nothing.
    enum { W = 512, H = 512 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    free(pixels);
    ASSERT(comp != NULL);

    char *content = lossy_deepq_content(100);
    ASSERT(content != NULL);
    size_t len = 0;
    uint8_t *pdf = lossy_make_content_image_pdf(comp, comp_len,
                                                "/Filter /FlateDecode",
                                                W, H, "DeviceGray", content, &len);
    free(content);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // The scan must see the true 500x500 pt placement.
    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(tspdf_lossy_scan_placements(doc, &pl, &n), TSPDF_OK);
    const TspdfLossyPlacement *im = lossy_find_placement(pl, n, 4);
    ASSERT(im != NULL);
    ASSERT(im->w_pt > 499.9 && im->w_pt < 500.1);
    ASSERT(im->h_pt > 499.9 && im->h_pt < 500.1);
    free(pl);

    // 512 px over 500 pt is ~74 dpi, well under the 150-dpi target: the
    // lossy pass must leave the image alone, never shrink it.
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *wobj = tspdf_dict_get(img_obj->stream.dict, "Width");
    ASSERT(wobj && wobj->integer == W);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_lossy_gstate_overflow_skips_image) {
    // Nesting past any sane depth (deeper than the gstate stack will grow):
    // the CTM is no longer trustworthy, so the image must be treated as
    // unmeasurable and skipped -- never downsampled.
    enum { W = 512, H = 512 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    free(pixels);
    ASSERT(comp != NULL);

    char *content = lossy_deepq_content(5000);
    ASSERT(content != NULL);
    size_t len = 0;
    uint8_t *pdf = lossy_make_content_image_pdf(comp, comp_len,
                                                "/Filter /FlateDecode",
                                                W, H, "DeviceGray", content, &len);
    free(content);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Unmeasurable: excluded from the placement list entirely.
    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(tspdf_lossy_scan_placements(doc, &pl, &n), TSPDF_OK);
    ASSERT(lossy_find_placement(pl, n, 4) == NULL);
    free(pl);

    // And the lossy pass leaves the stream byte-for-byte in place.
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *wobj = tspdf_dict_get(img_obj->stream.dict, "Width");
    ASSERT(wobj && wobj->integer == W);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_lossy_invalid_args_rejected) {
    size_t len = 0;
    char *pdf = lossy_make_ctm_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 0, 75, 300, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 0, 300, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 101, 300, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 0, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(NULL, 150, 75, 300, &st), TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_lossy_bilevel_downsample_threshold) {
    // 4x2 -> 2x1: each dest pixel is a 4x2... no, 2x2 window per dest.
    // Window 0 holds two black of four (tie -> black); window 1 none.
    uint8_t src[8] = {0, 255, 255, 255,
                      0, 255, 255, 255};
    uint8_t dst[2] = {9, 9};
    ASSERT(tspdf_lossy_bilevel_downsample(src, 4, 2, dst, 2, 1));
    ASSERT_EQ_INT(dst[0], 0);    // 2/4 black: tie goes to black
    ASSERT_EQ_INT(dst[1], 255);  // 0/4 black

    // Majority white wins when black is under half.
    uint8_t src2[8] = {0, 255, 255, 255,
                       255, 255, 255, 255};
    ASSERT(tspdf_lossy_bilevel_downsample(src2, 4, 2, dst, 2, 1));
    ASSERT_EQ_INT(dst[0], 255);  // 1/4 black
    ASSERT_EQ_INT(dst[1], 255);

    // Fractional 3 -> 2: floor boundaries give windows [0,1) and [1,3).
    uint8_t src3[3] = {0, 0, 255};
    uint8_t dst3[2] = {9, 9};
    ASSERT(tspdf_lossy_bilevel_downsample(src3, 3, 1, dst3, 2, 1));
    ASSERT_EQ_INT(dst3[0], 0);  // window {0}: black
    ASSERT_EQ_INT(dst3[1], 0);  // window {0,255}: tie -> black

    // Bad arguments.
    ASSERT(!tspdf_lossy_bilevel_downsample(NULL, 4, 2, dst, 2, 1));
    ASSERT(!tspdf_lossy_bilevel_downsample(src, 0, 2, dst, 2, 1));
    ASSERT(!tspdf_lossy_bilevel_downsample(src, 4, 2, dst, 0, 1));
}

TEST(test_lossy_mono_flate_downsampled_to_g4) {
    // 512x512 1-bpc flate "scan" drawn at 1x1 inch = 512 dpi; at mono
    // target 300 it must shrink to 300x300 and come out as CCITT G4.
    enum { W = 512, H = 512 };
    uint8_t *px = lossy_mono_text_pixels(W, H);
    ASSERT(px != NULL);
    size_t packed_len = 0;
    uint8_t *packed = lossy_mono_pack(px, W, H, &packed_len);
    ASSERT(packed != NULL);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(packed, packed_len, &comp_len);
    free(packed);
    ASSERT(comp != NULL);

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf_bpc(comp, comp_len, "/Filter /FlateDecode",
                                            W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);
    ASSERT_EQ_SIZE(st.images_mono, 1);
    ASSERT_EQ_SIZE(st.images_skipped, 0);
    ASSERT(st.bytes_after < st.bytes_before);

    uint8_t *out_px = NULL;
    ASSERT(lossy_mono_check_g4(doc, 300, 300, &out_px));
    // The downsampled page must still be text-shaped: both colors present,
    // black in a plausible band (the source is ~15-30% black).
    size_t black = 0;
    for (size_t i = 0; i < 300u * 300u; i++)
        if (out_px[i] == 0) black++;
    ASSERT(black > 300u * 300u / 20 && black < 300u * 300u / 2);
    free(out_px);

    // Round-trips through the compress-style save.
    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts), TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(out);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(px);
}

TEST(test_lossy_mono_flate_reencoded_1to1_below_target) {
    // A 1-bpc flate image already at target dpi is still converted to G4
    // (when >= 10% smaller) — without downsampling, so the pixels must
    // round-trip exactly.
    enum { W = 300, H = 300 };
    uint8_t *px = lossy_mono_text_pixels(W, H);
    ASSERT(px != NULL);
    size_t packed_len = 0;
    uint8_t *packed = lossy_mono_pack(px, W, H, &packed_len);
    ASSERT(packed != NULL);
    size_t comp_len = 0;
    // Compress level: deflate_compress is whatever the library does; the
    // text bitmap G4-encodes far below the flate size either way.
    uint8_t *comp = deflate_compress(packed, packed_len, &comp_len);
    free(packed);
    ASSERT(comp != NULL);

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf_bpc(comp, comp_len, "/Filter /FlateDecode",
                                            W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);
    ASSERT_EQ_SIZE(st.images_mono, 1);

    uint8_t *out_px = NULL;
    ASSERT(lossy_mono_check_g4(doc, W, H, &out_px));
    ASSERT(memcmp(out_px, px, (size_t)W * H) == 0);  // 1:1: bit-exact
    free(out_px);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(px);
}

TEST(test_lossy_mono_ccitt_downsampled) {
    // A 600-dpi G4 image (600x600 at 1x1 inch) must be downsampled to
    // 300x300 and stay CCITT.
    enum { W = 600, H = 600 };
    uint8_t *px = lossy_mono_text_pixels(W, H);
    ASSERT(px != NULL);
    TspdfArena enc_arena = tspdf_arena_create(1 << 20);
    uint8_t *g4 = NULL;
    size_t g4_len = 0;
    ASSERT(tspdf_ccitt_encode_g4(px, W, H, &enc_arena, &g4, &g4_len));

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf_bpc(
        g4, g4_len,
        "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns 600 /Rows 600 >>",
        W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    tspdf_arena_destroy(&enc_arena);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);
    ASSERT_EQ_SIZE(st.images_mono, 1);
    ASSERT(st.bytes_after < st.bytes_before);

    uint8_t *out_px = NULL;
    ASSERT(lossy_mono_check_g4(doc, 300, 300, &out_px));
    // 600 -> 300 is an exact 2:1 majority vote of a stroke pattern: the
    // result must still contain both colors.
    bool has_black = false, has_white = false;
    for (size_t i = 0; i < 300u * 300u; i++) {
        if (out_px[i] == 0) has_black = true;
        else has_white = true;
    }
    ASSERT(has_black && has_white);
    free(out_px);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(px);
}

TEST(test_lossy_mono_ccitt_passthrough_below_target) {
    // A CCITT image at or below 1.3x the mono target passes through
    // byte-identical: 360x360 at 1x1 inch = 360 dpi <= 1.3 * 300.
    enum { W = 360, H = 360 };
    uint8_t *px = lossy_mono_text_pixels(W, H);
    ASSERT(px != NULL);
    TspdfArena enc_arena = tspdf_arena_create(1 << 20);
    uint8_t *g4 = NULL;
    size_t g4_len = 0;
    ASSERT(tspdf_ccitt_encode_g4(px, W, H, &enc_arena, &g4, &g4_len));

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf_bpc(
        g4, g4_len,
        "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns 360 /Rows 360 >>",
        W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_mono, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);

    // Untouched: dict still 360 wide, stream not rewritten in place.
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL && img_obj->type == TSPDF_OBJ_STREAM);
    ASSERT(!img_obj->stream.self_contained);
    TspdfObj *wobj = tspdf_dict_get(img_obj->stream.dict, "Width");
    ASSERT(wobj && wobj->integer == 360);
    // ... and the raw stream bytes in the source are the original G4.
    ASSERT_EQ_SIZE((size_t)img_obj->stream.raw_len, g4_len);
    ASSERT(memcmp(doc->data + img_obj->stream.raw_offset, g4, g4_len) == 0);

    tspdf_arena_destroy(&enc_arena);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(px);
}

TEST(test_lossy_mono_noise_keeps_original) {
    // Random bit noise: G4 re-encoding is (much) larger than the deflated
    // original, so the keep-unless-10%-smaller rule must leave it alone.
    enum { W = 300, H = 300 };
    uint8_t *px = (uint8_t *)malloc((size_t)W * H);
    ASSERT(px != NULL);
    uint32_t lcg = 777;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        px[i] = (lcg >> 24) & 1 ? 255 : 0;
    }
    size_t packed_len = 0;
    uint8_t *packed = lossy_mono_pack(px, W, H, &packed_len);
    ASSERT(packed != NULL);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(packed, packed_len, &comp_len);
    free(packed);
    ASSERT(comp != NULL);

    size_t len = 0;
    // 300x300 at 1x1 inch = 300 dpi -> 1:1 G4 attempt, which loses to flate.
    uint8_t *pdf = lossy_make_image_pdf_bpc(comp, comp_len, "/Filter /FlateDecode",
                                            W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && strcmp((const char *)filter->string.data, "FlateDecode") == 0);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(px);
}

TEST(test_lossy_mono_imagemask_and_nondefault_decode_excluded) {
    enum { W = 512, H = 512 };
    uint8_t *px = lossy_mono_text_pixels(W, H);
    ASSERT(px != NULL);
    TspdfArena enc_arena = tspdf_arena_create(1 << 20);
    uint8_t *g4 = NULL;
    size_t g4_len = 0;
    ASSERT(tspdf_ccitt_encode_g4(px, W, H, &enc_arena, &g4, &g4_len));
    free(px);

    // /ImageMask true stays excluded, exactly like the 8-bit path.
    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf_bpc(
        g4, g4_len,
        "/ImageMask true /Filter /CCITTFaxDecode "
        "/DecodeParms << /K -1 /Columns 512 /Rows 512 >>",
        W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);
    tspdf_reader_destroy(doc);
    free(pdf);

    // A non-default /Decode ([1 0], the common CCITT inversion) is passed
    // through untouched — conservative, same as the 8-bit path.
    pdf = lossy_make_image_pdf_bpc(
        g4, g4_len,
        "/Decode [1 0] /Filter /CCITTFaxDecode "
        "/DecodeParms << /K -1 /Columns 512 /Rows 512 >>",
        W, H, 1, "DeviceGray", 72.0, 72.0, &len);
    ASSERT(pdf != NULL);
    doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    memset(&st, 0, sizeof(st));
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, 300, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);
    tspdf_reader_destroy(doc);
    free(pdf);
    tspdf_arena_destroy(&enc_arena);
}

void run_lossy_tests(void) {
    printf("\n  Lossy image recompression:\n");
    RUN(test_lossy_box_downsample_exact);
    RUN(test_lossy_box_downsample_fractional_edges);
    RUN(test_lossy_gray_detect);
    RUN(test_lossy_target_dims);
    RUN(test_lossy_keep_original_rule);
    RUN(test_lossy_ctm_scan_placements);
    RUN(test_lossy_predictor15_flate_image_recompressed);
    RUN(test_lossy_rgb_near_gray_converts_to_devicegray);
    RUN(test_lossy_skips_low_dpi_smask_and_small);
    RUN(test_lossy_deep_q_nesting_measured_at_outer_ctm);
    RUN(test_lossy_gstate_overflow_skips_image);
    RUN(test_lossy_invalid_args_rejected);
    RUN(test_lossy_bilevel_downsample_threshold);
    RUN(test_lossy_mono_flate_downsampled_to_g4);
    RUN(test_lossy_mono_flate_reencoded_1to1_below_target);
    RUN(test_lossy_mono_ccitt_downsampled);
    RUN(test_lossy_mono_ccitt_passthrough_below_target);
    RUN(test_lossy_mono_noise_keeps_original);
    RUN(test_lossy_mono_imagemask_and_nondefault_decode_excluded);
}
