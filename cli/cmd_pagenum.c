#include "commands.h"
#include "../include/tspdf_overlay.h"
#include "../src/util/pdftext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    POS_BOTTOM_CENTER,
    POS_BOTTOM_LEFT,
    POS_BOTTOM_RIGHT,
    POS_TOP_CENTER,
    POS_TOP_LEFT,
    POS_TOP_RIGHT,
} PagenumPosition;

// The format string is handed to snprintf with two int arguments, so it must
// not smuggle in any other conversion. Allowed: literal text, "%%", and at
// most two plain "%d". Returns 0 and the %d count on success, -1 otherwise.
static int validate_format(const char *fmt, int *d_count) {
    int n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') { p++; continue; }
        if (p[1] == 'd') { n++; p++; continue; }
        return -1;  // any flag/width/length/other conversion is rejected
    }
    if (n > 2) return -1;
    *d_count = n;
    return 0;
}

int cmd_pagenum(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf pagenum <input.pdf> -o <output.pdf> [--format \"%%d / %%d\"]\n");
        printf("                     [--position <pos>] [--start N] [--font-size N]\n");
        printf("                     [--pages <range>]\n");
        printf("\nStamp a page number on every page.\n");
        printf("The format may contain up to two %%d: the page number and the total.\n");
        printf("Positions: bottom-center (default), bottom-left, bottom-right,\n");
        printf("           top-center, top-left, top-right\n");
        printf("With --pages (e.g. 2-10), only those pages are stamped; the number\n");
        printf("still reflects the true page position (useful to skip cover pages).\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf pagenum: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf pagenum: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf pagenum: missing -o <output.pdf>\n");
        return 1;
    }

    const char *format = find_flag(argc, argv, "--format");
    if (!format) format = "%d";
    int d_count = 0;
    if (validate_format(format, &d_count) != 0) {
        fprintf(stderr, "tspdf pagenum: invalid --format '%s' (only plain %%d, "
                        "at most twice, and %%%% are allowed)\n", format);
        return 1;
    }

    PagenumPosition pos = POS_BOTTOM_CENTER;
    const char *pos_str = find_flag(argc, argv, "--position");
    if (pos_str) {
        if      (strcmp(pos_str, "bottom-center") == 0) pos = POS_BOTTOM_CENTER;
        else if (strcmp(pos_str, "bottom-left")   == 0) pos = POS_BOTTOM_LEFT;
        else if (strcmp(pos_str, "bottom-right")  == 0) pos = POS_BOTTOM_RIGHT;
        else if (strcmp(pos_str, "top-center")    == 0) pos = POS_TOP_CENTER;
        else if (strcmp(pos_str, "top-left")      == 0) pos = POS_TOP_LEFT;
        else if (strcmp(pos_str, "top-right")     == 0) pos = POS_TOP_RIGHT;
        else {
            fprintf(stderr, "tspdf pagenum: invalid --position '%s'\n", pos_str);
            return 1;
        }
    }

    long start = 1;
    const char *start_str = find_flag(argc, argv, "--start");
    if (start_str) {
        char *end = NULL;
        start = strtol(start_str, &end, 10);
        if (end == start_str || *end != '\0' || start < 0 || start > 1000000000L) {
            fprintf(stderr, "tspdf pagenum: invalid --start '%s'\n", start_str);
            return 1;
        }
    }

    double font_size = 10.0;
    const char *size_str = find_flag(argc, argv, "--font-size");
    if (size_str) {
        font_size = atof(size_str);
        if (font_size <= 0 || font_size > 288) {
            fprintf(stderr, "tspdf pagenum: --font-size must be between 0 and 288\n");
            return 1;
        }
    }

    // --pages restricts which pages get a stamp (e.g. skip cover pages);
    // the printed number still reflects the true page position.
    size_t sel_count = 0;
    size_t *sel = NULL;
    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        sel = parse_page_range(pages_spec, &sel_count);
        if (!sel) {
            fprintf(stderr, "tspdf pagenum: invalid page range '%s'\n", pages_spec);
            return 1;
        }
    }

    // The stamp is drawn with the built-in Helvetica font (WinAnsi/cp1252);
    // re-encode the format up front, same as cmd_watermark does for --text.
    char *wa_format = malloc(strlen(format) + 1);
    if (!wa_format) {
        fprintf(stderr, "tspdf pagenum: out of memory\n");
        free(sel);
        return 1;
    }
    uint32_t bad_cp = 0;
    int conv = tspdf_utf8_to_cp1252(format, wa_format, &bad_cp);
    if (conv != TSPDF_PDFTEXT_OK) {
        if (conv == TSPDF_PDFTEXT_UNMAPPED) {
            char ch[5];
            ch[tspdf_utf8_encode(bad_cp, ch)] = '\0';
            fprintf(stderr, "tspdf pagenum: character '%s' (U+%04X) cannot be shown with the "
                            "built-in fonts (WinAnsi/cp1252); use Latin-script text\n",
                    ch, (unsigned)bad_cp);
        } else {
            fprintf(stderr, "tspdf pagenum: --format is not valid UTF-8\n");
        }
        free(wa_format);
        free(sel);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf pagenum: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(wa_format);
        free(sel);
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);
    long total = start + (long)page_count - 1;
    double margin = 30.0;

    // Turn the selected pages into a per-page mask; NULL means "stamp all".
    // (Checked index by index: these values index the mask below, so an
    // out-of-range entry must never slip through.)
    bool *stamp = NULL;
    if (sel) {
        for (size_t k = 0; k < sel_count; k++) {
            if (sel[k] >= page_count) {
                fprintf(stderr, "tspdf pagenum: page %zu is out of range (document has %zu page%s)\n",
                        sel[k] + 1, page_count, page_count == 1 ? "" : "s");
                tspdf_reader_destroy(doc);
                free(wa_format);
                free(sel);
                return 1;
            }
        }
        stamp = calloc(page_count, sizeof(bool));
        if (!stamp) {
            fprintf(stderr, "tspdf pagenum: out of memory\n");
            tspdf_reader_destroy(doc);
            free(wa_format);
            free(sel);
            return 1;
        }
        for (size_t k = 0; k < sel_count; k++) stamp[sel[k]] = true;
    }

    size_t stamped = 0;
    for (size_t i = 0; i < page_count; i++) {
        if (stamp && !stamp[i]) continue;
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        double x0 = page->media_box[0];
        double y0 = page->media_box[1];
        double w = page->media_box[2] - page->media_box[0];
        double h = page->media_box[3] - page->media_box[1];

        // Both int arguments are always passed; snprintf ignores the excess
        // when the (validated) format consumes fewer (C11 7.21.6.1p2).
        char label[256];
        snprintf(label, sizeof(label), wa_format, (int)(start + (long)i), (int)total);

        TspdfWriter *writer = tspdf_writer_create();
        if (!writer) continue;

        const char *font_name = tspdf_writer_add_builtin_font(writer, "Helvetica");
        if (!font_name) {
            tspdf_writer_destroy(writer);
            continue;
        }

        double text_width = tspdf_writer_measure_text(writer, font_name, font_size, label);
        if (text_width <= 0) text_width = (double)strlen(label) * font_size * 0.5;

        double x, y;
        switch (pos) {
            case POS_BOTTOM_LEFT:  case POS_TOP_LEFT:  x = margin; break;
            case POS_BOTTOM_RIGHT: case POS_TOP_RIGHT: x = w - margin - text_width; break;
            default:                                   x = (w - text_width) / 2.0; break;
        }
        switch (pos) {
            case POS_TOP_CENTER: case POS_TOP_LEFT: case POS_TOP_RIGHT:
                y = h - margin - font_size;  // baseline one line below the top margin
                break;
            default:
                y = margin;
                break;
        }

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) {
            tspdf_writer_destroy(writer);
            continue;
        }

        tspdf_stream_save(stream);
        tspdf_stream_set_fill_color(stream, tspdf_color_rgb(0, 0, 0));
        tspdf_stream_begin_text(stream);
        tspdf_stream_set_font(stream, font_name, font_size);
        tspdf_stream_text_position(stream, x0 + x, y0 + y);
        tspdf_stream_show_text(stream, label);
        tspdf_stream_end_text(stream);
        tspdf_stream_restore(stream);

        err = tspdf_page_end_content(doc, i, stream, writer);
        tspdf_writer_destroy(writer);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf pagenum: overlay failed on page %zu: %s\n",
                    i + 1, tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            free(wa_format);
            free(sel);
            free(stamp);
            return 1;
        }
        stamped++;
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf pagenum: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(wa_format);
        free(sel);
        free(stamp);
        return 1;
    }

    printf("Numbered %zu of %zu page(s) → %s\n", stamped, page_count, output);

    tspdf_reader_destroy(doc);
    free(wa_format);
    free(sel);
    free(stamp);
    return 0;
}
