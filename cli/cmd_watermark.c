#include "commands.h"
#include "../include/tspdf_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int cmd_watermark(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf watermark <input.pdf> -o <output.pdf> --text \"DRAFT\" [--opacity 0.3]\n");
        printf("\nAdd a diagonal text watermark to all pages.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf watermark: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf watermark: missing -o <output.pdf>\n");
        return 1;
    }

    const char *text = find_flag(argc, argv, "--text");
    if (!text) {
        fprintf(stderr, "tspdf watermark: missing --text <text>\n");
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

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf watermark: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);
    double font_size = 48.0;
    double angle_rad = 45.0 * M_PI / 180.0;
    double cos_a = cos(angle_rad);
    double sin_a = sin(angle_rad);

    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        double w = page->media_box[2] - page->media_box[0];
        double h = page->media_box[3] - page->media_box[1];
        double cx = w / 2.0;
        double cy = h / 2.0;

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
        double text_width = (double)strlen(text) * font_size * 0.5;

        tspdf_stream_begin_text(stream);
        tspdf_stream_set_font(stream, font_name, font_size);
        tspdf_stream_text_position(stream, -text_width / 2.0, -font_size / 3.0);
        tspdf_stream_show_text(stream, text);
        tspdf_stream_end_text(stream);

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
