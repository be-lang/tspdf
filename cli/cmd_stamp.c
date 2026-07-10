// tspdf stamp: overlay a page of one PDF onto pages of another (pdftk/qpdf
// stamp/overlay). The stamp page is imported once as a self-contained /Form
// XObject, then drawn on every selected page scaled to fit (aspect preserved,
// centered), compensating a page-level /Rotate so the stamp reads upright.

#include "commands.h"
#include "../include/tspdf_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Concat a rotation by `rot` degrees CCW about (cx, cy) — compensates the
// viewer's clockwise /Rotate so subsequent drawing is upright as viewed.
// No-op for rot == 0.
void tspdf_cli_emit_rotate_compensation(TspdfStream *s, int rot, double cx, double cy) {
    double a, b;
    switch (((rot % 360) + 360) % 360) {
        case 90:  a = 0.0;  b = 1.0;  break;
        case 180: a = -1.0; b = 0.0;  break;
        case 270: a = 0.0;  b = -1.0; break;
        default: return;
    }
    tspdf_stream_concat_matrix(s, a, b, -b, a,
                               cx - a * cx + b * cy,
                               cy - b * cx - a * cy);
}

static void print_stamp_help(void) {
    printf("Usage: tspdf stamp <input.pdf> --stamp <stamp.pdf> -o <output.pdf>\n");
    printf("                   [--pages <range>] [--under] [--stamp-page N]\n");
    printf("                   [--password <pass>] [--stamp-password <pass>]\n");
    printf("\nOverlay a page of stamp.pdf onto pages of input.pdf.\n");
    printf("The stamp page is scaled to fit each target page (aspect preserved,\n");
    printf("centered). By default it is drawn on top of the existing content;\n");
    printf("--under puts it beneath instead (a letterhead/background).\n");
}

int cmd_stamp(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_stamp_help();
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf stamp: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf stamp: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf stamp: missing -o <output.pdf>\n");
        return 1;
    }

    const char *stamp_path = find_flag(argc, argv, "--stamp");
    if (!stamp_path) {
        fprintf(stderr, "tspdf stamp: missing --stamp <stamp.pdf>\n");
        return 1;
    }

    long stamp_page = 1;
    const char *sp_str = find_flag(argc, argv, "--stamp-page");
    if (sp_str) {
        char *end = NULL;
        stamp_page = strtol(sp_str, &end, 10);
        if (end == sp_str || *end != '\0' || stamp_page < 1) {
            fprintf(stderr, "tspdf stamp: invalid --stamp-page '%s'\n", sp_str);
            return 1;
        }
    }

    bool under = has_flag(argc, argv, "--under");
    const char *password = find_flag(argc, argv, "--password");
    const char *stamp_password = find_flag(argc, argv, "--stamp-password");

    size_t sel_count = 0;
    size_t *sel = NULL;
    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        sel = parse_page_range(pages_spec, &sel_count);
        if (!sel) {
            fprintf(stderr, "tspdf stamp: invalid page range '%s'\n", pages_spec);
            return 1;
        }
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf stamp: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(sel);
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);
    if (sel) {
        size_t bad = first_out_of_range(sel, sel_count, page_count);
        if (bad != page_count) {
            fprintf(stderr, "tspdf stamp: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, page_count, page_count == 1 ? "" : "s");
            tspdf_reader_destroy(doc);
            free(sel);
            return 1;
        }
    }

    TspdfReader *stamp_doc = stamp_password
        ? tspdf_reader_open_file_with_password(stamp_path, stamp_password, &err)
        : tspdf_reader_open_file(stamp_path, &err);
    if (!stamp_doc) {
        fprintf(stderr, "tspdf stamp: failed to open '%s': %s\n", stamp_path, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(sel);
        return 1;
    }

    size_t stamp_pages = tspdf_reader_page_count(stamp_doc);
    if ((size_t)stamp_page > stamp_pages) {
        fprintf(stderr, "tspdf stamp: --stamp-page %ld is out of range ('%s' has %zu page%s)\n",
                stamp_page, stamp_path, stamp_pages, stamp_pages == 1 ? "" : "s");
        tspdf_reader_destroy(stamp_doc);
        tspdf_reader_destroy(doc);
        free(sel);
        return 1;
    }

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(doc, stamp_doc,
                                                     (size_t)(stamp_page - 1), bbox, &err);
    // The import is self-contained: the stamp document is not needed anymore.
    tspdf_reader_destroy(stamp_doc);
    if (xnum == 0) {
        fprintf(stderr, "tspdf stamp: failed to import stamp page: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(sel);
        return 1;
    }

    double bw = bbox[2] - bbox[0];
    double bh = bbox[3] - bbox[1];

    bool *mask = NULL;
    if (sel) {
        mask = calloc(page_count, sizeof(bool));
        if (!mask) {
            fprintf(stderr, "tspdf stamp: out of memory\n");
            tspdf_reader_destroy(doc);
            free(sel);
            return 1;
        }
        for (size_t k = 0; k < sel_count; k++) mask[sel[k]] = true;
    }

    size_t stamped = 0;
    for (size_t i = 0; i < page_count; i++) {
        if (mask && !mask[i]) continue;
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        double pw = page->media_box[2] - page->media_box[0];
        double ph = page->media_box[3] - page->media_box[1];
        if (pw <= 0 || ph <= 0) continue;

        // Fit in the VIEWED orientation: /Rotate 90/270 swaps the axes.
        int rot = ((page->rotate % 360) + 360) % 360;
        double vw = (rot == 90 || rot == 270) ? ph : pw;
        double vh = (rot == 90 || rot == 270) ? pw : ph;

        double s = vw / bw;
        double s2 = vh / bh;
        if (s2 < s) s = s2;
        if (!(s > 0)) continue;

        double cx = (page->media_box[0] + page->media_box[2]) / 2.0;
        double cy = (page->media_box[1] + page->media_box[3]) / 2.0;

        const char *name = tspdf_page_add_xobject(doc, i, xnum);
        if (!name) continue;

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) continue;

        tspdf_stream_save(stream);
        tspdf_cli_emit_rotate_compensation(stream, rot, cx, cy);
        // Center the scaled BBox on the page center (the form's BBox origin
        // is not necessarily 0,0).
        tspdf_stream_draw_image(stream, name,
                                cx - s * (bbox[0] + bbox[2]) / 2.0,
                                cy - s * (bbox[1] + bbox[3]) / 2.0,
                                s, s);
        tspdf_stream_restore(stream);

        err = under
            ? tspdf_page_end_content_under(doc, i, stream, NULL)
            : tspdf_page_end_content(doc, i, stream, NULL);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf stamp: overlay failed on page %zu: %s\n",
                    i + 1, tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            free(sel);
            free(mask);
            return 1;
        }
        stamped++;
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf stamp: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(sel);
        free(mask);
        return 1;
    }

    printf("Stamped %zu of %zu page(s) → %s\n", stamped, page_count, output);

    tspdf_reader_destroy(doc);
    free(sel);
    free(mask);
    return 0;
}
