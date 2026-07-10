#include "commands.h"
#include "../include/tspdf_overlay.h"
#include "../src/util/pdftext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum {
    WM_POS_CENTER,
    WM_POS_TILE,
    WM_POS_TOP_LEFT,
    WM_POS_TOP_RIGHT,
    WM_POS_BOTTOM_LEFT,
    WM_POS_BOTTOM_RIGHT,
} WatermarkPosition;

static void print_watermark_help(void) {
    printf("Usage: tspdf watermark <input.pdf> -o <output.pdf> --text \"DRAFT\" [--opacity 0.3]\n");
    printf("       tspdf watermark <input.pdf> -o <output.pdf> --image <logo.png|jpg>\n");
    printf("                       [--opacity 0.3] [--scale 0.5] [--position <pos>]\n");
    printf("\nAdd a watermark to all pages: diagonal text, or an image (PNG/JPEG).\n");
    printf("Positions: center (default), tile, top-left, top-right,\n");
    printf("           bottom-left, bottom-right\n");
}

// Build a one-page PDF holding just the image at its pixel size (72 dpi) and
// return the reader-openable bytes. PNG or JPEG, sniffed by magic bytes.
static uint8_t *build_image_pdf(const char *path, size_t *out_len,
                                const char **fail_reason) {
    *fail_reason = "cannot read file";
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t magic[2] = {0, 0};
    size_t got = fread(magic, 1, 2, f);
    fclose(f);
    if (got != 2) return NULL;

    bool is_png = (magic[0] == 0x89 && magic[1] == 'P');
    bool is_jpeg = (magic[0] == 0xFF && magic[1] == 0xD8);
    if (!is_png && !is_jpeg) {
        *fail_reason = "not a PNG or JPEG file";
        return NULL;
    }

    TspdfWriter *w = tspdf_writer_create();
    if (!w) {
        *fail_reason = "out of memory";
        return NULL;
    }
    const char *name = is_png ? tspdf_writer_add_png_image(w, path)
                              : tspdf_writer_add_jpeg_image(w, path);
    if (!name) {
        *fail_reason = tspdf_error_string(tspdf_writer_last_error(w));
        tspdf_writer_destroy(w);
        return NULL;
    }

    TspdfImage *img = &w->images[w->image_count - 1];
    double iw = (double)img->width;
    double ih = (double)img->height;
    if (iw <= 0 || ih <= 0) {
        *fail_reason = "image has no pixels";
        tspdf_writer_destroy(w);
        return NULL;
    }

    TspdfStream *page = tspdf_writer_add_page_sized(w, iw, ih);
    if (!page) {
        *fail_reason = "out of memory";
        tspdf_writer_destroy(w);
        return NULL;
    }
    tspdf_stream_draw_image(page, name, 0, 0, iw, ih);

    uint8_t *data = NULL;
    *out_len = 0;
    TspdfError err = tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    if (err != TSPDF_OK) {
        *fail_reason = tspdf_error_string(err);
        return NULL;
    }
    return data;
}

static int watermark_image(const char *input, const char *output, const char *image_path,
                           double opacity, double scale, WatermarkPosition pos) {
    const char *fail_reason = NULL;
    size_t img_pdf_len = 0;
    uint8_t *img_pdf = build_image_pdf(image_path, &img_pdf_len, &fail_reason);
    if (!img_pdf) {
        fprintf(stderr, "tspdf watermark: failed to load image '%s': %s\n",
                image_path, fail_reason);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf watermark: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(img_pdf);
        return 1;
    }

    TspdfReader *img_doc = tspdf_reader_open(img_pdf, img_pdf_len, &err);
    if (!img_doc) {
        fprintf(stderr, "tspdf watermark: internal error preparing image page: %s\n",
                tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(img_pdf);
        return 1;
    }

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(doc, img_doc, 0, bbox, &err);
    tspdf_reader_destroy(img_doc);
    free(img_pdf);
    if (xnum == 0) {
        fprintf(stderr, "tspdf watermark: failed to import image page: %s\n",
                tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    double bw = bbox[2] - bbox[0];
    double bh = bbox[3] - bbox[1];

    size_t page_count = tspdf_reader_page_count(doc);
    const double margin = 36.0;

    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        double pw = page->media_box[2] - page->media_box[0];
        double ph = page->media_box[3] - page->media_box[1];
        if (pw <= 0 || ph <= 0) continue;

        // Work in the VIEWED orientation (see cmd_stamp.c): compute the
        // placement in view space, then pre-rotate by /Rotate about the page
        // center so the image reads upright as viewed.
        int rot = ((page->rotate % 360) + 360) % 360;
        double vw = (rot == 90 || rot == 270) ? ph : pw;
        double vh = (rot == 90 || rot == 270) ? pw : ph;

        double cx = (page->media_box[0] + page->media_box[2]) / 2.0;
        double cy = (page->media_box[1] + page->media_box[3]) / 2.0;
        double vox = cx - vw / 2.0;  // view-box origin after rotation compensation
        double voy = cy - vh / 2.0;

        // Larger image dimension = scale * min(page dims).
        double target = scale * (vw < vh ? vw : vh);
        double larger = bw > bh ? bw : bh;
        double s = target / larger;
        if (!(s > 0)) continue;
        double dw = s * bw;
        double dh = s * bh;

        const char *name = tspdf_page_add_xobject(doc, i, xnum);
        if (!name) continue;

        TspdfWriter *writer = tspdf_writer_create();
        if (!writer) continue;
        const char *gs_name = tspdf_writer_add_opacity(writer, opacity, opacity);

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) {
            tspdf_writer_destroy(writer);
            continue;
        }

        tspdf_stream_save(stream);
        if (gs_name) tspdf_stream_set_opacity(stream, gs_name);
        tspdf_cli_emit_rotate_compensation(stream, rot, cx, cy);

        // draw_image maps the form BBox through [s 0 0 s x y]; shift by the
        // BBox origin so (x, y) is the visual bottom-left corner.
        double ox = s * bbox[0];
        double oy = s * bbox[1];

        if (pos == WM_POS_TILE) {
            double step_x = dw * 1.5;
            double step_y = dh * 1.5;
            int drawn = 0;
            for (double y = voy; y < voy + vh && drawn < 10000; y += step_y) {
                for (double x = vox; x < vox + vw && drawn < 10000; x += step_x) {
                    tspdf_stream_draw_image(stream, name, x - ox, y - oy, s, s);
                    drawn++;
                }
            }
        } else {
            double x, y;
            switch (pos) {
                case WM_POS_TOP_LEFT:     x = vox + margin;           y = voy + vh - margin - dh; break;
                case WM_POS_TOP_RIGHT:    x = vox + vw - margin - dw; y = voy + vh - margin - dh; break;
                case WM_POS_BOTTOM_LEFT:  x = vox + margin;           y = voy + margin;           break;
                case WM_POS_BOTTOM_RIGHT: x = vox + vw - margin - dw; y = voy + margin;           break;
                default:                  x = cx - dw / 2.0;          y = cy - dh / 2.0;          break;
            }
            tspdf_stream_draw_image(stream, name, x - ox, y - oy, s, s);
        }

        tspdf_stream_restore(stream);

        err = tspdf_page_end_content(doc, i, stream, writer);
        tspdf_writer_destroy(writer);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf watermark: overlay failed on page %zu: %s\n",
                    i + 1, tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            return 1;
        }
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf watermark: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("Watermarked %zu page(s) → %s\n", page_count, output);
    tspdf_reader_destroy(doc);
    return 0;
}

static int watermark_text(const char *input, const char *output, const char *text,
                          double opacity) {
    // The watermark is drawn with the built-in Helvetica font, which uses
    // WinAnsiEncoding (cp1252): re-encode the UTF-8 input up front so the
    // content stream carries cp1252 bytes instead of raw UTF-8 mojibake.
    char *wa_text = malloc(strlen(text) + 1);
    if (!wa_text) {
        fprintf(stderr, "tspdf watermark: out of memory\n");
        return 1;
    }
    uint32_t bad_cp = 0;
    int conv = tspdf_utf8_to_cp1252(text, wa_text, &bad_cp);
    if (conv != TSPDF_PDFTEXT_OK) {
        if (conv == TSPDF_PDFTEXT_UNMAPPED) {
            char ch[5];
            ch[tspdf_utf8_encode(bad_cp, ch)] = '\0';
            fprintf(stderr, "tspdf watermark: character '%s' (U+%04X) cannot be shown with the "
                            "built-in fonts (WinAnsi/cp1252); use Latin-script text\n",
                    ch, (unsigned)bad_cp);
        } else {
            fprintf(stderr, "tspdf watermark: --text is not valid UTF-8\n");
        }
        free(wa_text);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf watermark: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(wa_text);
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);
    double font_size = 48.0;

    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        // Center of the VISIBLE page: the MediaBox origin is not always
        // (0,0), so the center is ((x0+x1)/2, (y0+y1)/2), not (w/2, h/2).
        double cx = (page->media_box[0] + page->media_box[2]) / 2.0;
        double cy = (page->media_box[1] + page->media_box[3]) / 2.0;

        // Compensate a page-level /Rotate: the viewer turns the page
        // clockwise by that many degrees, so pre-rotate the stamp the other
        // way (45 + rotate CCW) to keep the diagonal upright as viewed.
        // Rotation is about the page center, so the stamp stays centered.
        int rot = ((page->rotate % 360) + 360) % 360;
        double angle_rad = (45.0 + (double)rot) * M_PI / 180.0;
        double cos_a = cos(angle_rad);
        double sin_a = sin(angle_rad);

        TspdfWriter *writer = tspdf_writer_create();
        if (!writer) continue;

        const char *font_name = tspdf_writer_add_builtin_font(writer, "Helvetica");
        if (!font_name) {
            tspdf_writer_destroy(writer);
            continue;
        }

        // Add opacity state
        const char *gs_name = tspdf_writer_add_opacity(writer, opacity, opacity);

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) {
            tspdf_writer_destroy(writer);
            continue;
        }

        tspdf_stream_save(stream);

        // Set opacity if we got a graphics state
        if (gs_name) {
            tspdf_stream_set_opacity(stream, gs_name);
        }

        // Set gray color for watermark
        TspdfColor gray = tspdf_color_rgb(0.7, 0.7, 0.7);
        tspdf_stream_set_fill_color(stream, gray);

        // Transform: translate to center, then rotate 45 degrees
        tspdf_stream_concat_matrix(stream, cos_a, sin_a, -sin_a, cos_a, cx, cy);

        // Estimate text width to center it (rough: ~0.5 * font_size per char for Helvetica)
        double text_width = (double)strlen(wa_text) * font_size * 0.5;

        tspdf_stream_begin_text(stream);
        tspdf_stream_set_font(stream, font_name, font_size);
        tspdf_stream_text_position(stream, -text_width / 2.0, -font_size / 3.0);
        tspdf_stream_show_text(stream, wa_text);
        tspdf_stream_end_text(stream);

        tspdf_stream_restore(stream);

        err = tspdf_page_end_content(doc, i, stream, writer);
        tspdf_writer_destroy(writer);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf watermark: overlay failed on page %zu: %s\n",
                    i + 1, tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            free(wa_text);
            return 1;
        }
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf watermark: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(wa_text);
        return 1;
    }

    printf("Watermarked %zu page(s) → %s\n", page_count, output);

    tspdf_reader_destroy(doc);
    free(wa_text);
    return 0;
}

int cmd_watermark(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_watermark_help();
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf watermark: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf watermark: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf watermark: missing -o <output.pdf>\n");
        return 1;
    }

    const char *text = find_flag(argc, argv, "--text");
    const char *image = find_flag(argc, argv, "--image");
    if (text && image) {
        fprintf(stderr, "tspdf watermark: --text and --image are mutually exclusive\n");
        return 1;
    }
    if (!text && !image) {
        fprintf(stderr, "tspdf watermark: missing --text <text> or --image <file>\n");
        return 1;
    }

    double opacity = 0.3;
    const char *opacity_str = find_flag(argc, argv, "--opacity");
    if (opacity_str) {
        opacity = atof(opacity_str);
        if (opacity <= 0.0 || opacity > 1.0) {
            fprintf(stderr, "tspdf watermark: opacity must be between 0 and 1\n");
            return 1;
        }
    }

    if (text) {
        return watermark_text(input, output, text, opacity);
    }

    double scale = 0.5;
    const char *scale_str = find_flag(argc, argv, "--scale");
    if (scale_str) {
        scale = atof(scale_str);
        if (scale <= 0.0 || scale > 10.0) {
            fprintf(stderr, "tspdf watermark: --scale must be between 0 and 10\n");
            return 1;
        }
    }

    WatermarkPosition pos = WM_POS_CENTER;
    const char *pos_str = find_flag(argc, argv, "--position");
    if (pos_str) {
        if      (strcmp(pos_str, "center") == 0)       pos = WM_POS_CENTER;
        else if (strcmp(pos_str, "tile") == 0)         pos = WM_POS_TILE;
        else if (strcmp(pos_str, "top-left") == 0)     pos = WM_POS_TOP_LEFT;
        else if (strcmp(pos_str, "top-right") == 0)    pos = WM_POS_TOP_RIGHT;
        else if (strcmp(pos_str, "bottom-left") == 0)  pos = WM_POS_BOTTOM_LEFT;
        else if (strcmp(pos_str, "bottom-right") == 0) pos = WM_POS_BOTTOM_RIGHT;
        else {
            fprintf(stderr, "tspdf watermark: invalid --position '%s'\n", pos_str);
            return 1;
        }
    }

    return watermark_image(input, output, image, opacity, scale, pos);
}
