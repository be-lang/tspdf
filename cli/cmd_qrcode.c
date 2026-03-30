#include "commands.h"
#include "../include/tspdf.h"
#include "../src/qr/qr_encode.h"
#include <stdio.h>
#include <string.h>

int cmd_qrcode(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf qrcode <text> -o <output.pdf> [--title <title>] [--subtitle <sub>] [--no-link]\n");
        printf("\nGenerate a PDF with a QR code.\n");
        printf("  --no-link    Hide the URL/text below the QR code\n");
        return argc == 0 ? 1 : 0;
    }

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf qrcode: missing -o <output.pdf>\n");
        return 1;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    /* Discard positional if it is actually the value of a flag */
    if (npos == 1 && (positional[0] == output ||
                      positional[0] == find_flag(argc, argv, "--title") ||
                      positional[0] == find_flag(argc, argv, "--subtitle"))) {
        npos = 0;
    }
    if (npos < 1) {
        fprintf(stderr, "tspdf qrcode: missing text/URL to encode\n");
        return 1;
    }
    const char *text = positional[0];

    const char *title = find_flag(argc, argv, "--title");
    const char *subtitle = find_flag(argc, argv, "--subtitle");
    bool show_link = !has_flag(argc, argv, "--no-link");

    QrCode *qr = qr_encode(text);
    if (!qr) {
        fprintf(stderr, "tspdf qrcode: failed to encode QR code (text too long?)\n");
        return 1;
    }

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { qr_free(qr); fprintf(stderr, "tspdf qrcode: out of memory\n"); return 1; }

    if (title) tspdf_writer_set_title(doc, title);
    tspdf_writer_set_creator(doc, "tspdf");

    const char *sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");

    double W = TSPDF_PAGE_A4_WIDTH, H = TSPDF_PAGE_A4_HEIGHT;
    TspdfStream *page = tspdf_writer_add_page(doc);

    /* White background */
    tspdf_stream_set_fill_color(page, tspdf_color_rgb(1, 1, 1));
    tspdf_stream_rect(page, 0, 0, W, H);
    tspdf_stream_fill(page);

    /* QR code: 200pt square, centered, slightly above center */
    double qr_pt = 200.0;
    double cell_size = qr_pt / (double)qr->size;
    double qr_x = (W - qr_pt) / 2.0;
    double qr_y = (H - qr_pt) / 2.0 - 20.0;

    /* Draw dark modules */
    tspdf_stream_set_fill_color(page, tspdf_color_rgb(0, 0, 0));
    for (int row = 0; row < qr->size; row++) {
        for (int col = 0; col < qr->size; col++) {
            if (qr->modules[row * qr->size + col]) {
                double px = qr_x + col * cell_size;
                double py = qr_y + (qr->size - 1 - row) * cell_size;
                tspdf_stream_rect(page, px, py, cell_size, cell_size);
                tspdf_stream_fill(page);
            }
        }
    }

    /* Border around QR */
    tspdf_stream_set_stroke_color(page, tspdf_color_from_u8(200, 205, 220));
    tspdf_stream_set_line_width(page, 1.0);
    tspdf_stream_rect(page, qr_x - 8, qr_y - 8, qr_pt + 16, qr_pt + 16);
    tspdf_stream_stroke(page);

    /* Title above QR (if present) */
    if (title && title[0]) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, bold, 22.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(20, 25, 60));
        double title_w = tspdf_writer_measure_text(doc, bold, 22.0, title);
        tspdf_stream_text_position(page, (W - title_w) / 2.0, qr_y + qr_pt + 32.0);
        tspdf_stream_show_text(page, title);
        tspdf_stream_end_text(page);
    }

    /* URL/text below QR */
    if (show_link) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, sans, 11.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(79, 110, 247));
        double text_w = tspdf_writer_measure_text(doc, sans, 11.0, text);
        tspdf_stream_text_position(page, (W - text_w) / 2.0, qr_y - 20.0);
        tspdf_stream_show_text(page, text);
        tspdf_stream_end_text(page);
    }

    /* Subtitle (if present) */
    if (subtitle && subtitle[0]) {
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, sans, 10.0);
        tspdf_stream_set_fill_color(page, tspdf_color_from_u8(130, 140, 170));
        double sub_w = tspdf_writer_measure_text(doc, sans, 10.0, subtitle);
        tspdf_stream_text_position(page, (W - sub_w) / 2.0, qr_y - 36.0);
        tspdf_stream_show_text(page, subtitle);
        tspdf_stream_end_text(page);
    }

    qr_free(qr);

    TspdfError err = tspdf_writer_save(doc, output);
    tspdf_writer_destroy(doc);

    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf qrcode: failed to save '%s': %s\n", output, tspdf_error_string(err));
        return 1;
    }

    printf("QR code -> %s\n", output);
    return 0;
}
