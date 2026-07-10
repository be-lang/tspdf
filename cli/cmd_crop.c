// tspdf crop: set the /CropBox of pages (non-destructive — content is kept and
// merely clipped to the visible region). Either an explicit box (relative to
// each page's MediaBox origin) or inward margins.

#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Parse "a,b,c,d" into exactly `n` doubles. Returns true on success.
static bool parse_csv_doubles(const char *s, double *out, int n) {
    if (!s) return false;
    for (int i = 0; i < n; i++) {
        char *end = NULL;
        out[i] = strtod(s, &end);
        if (end == s) return false;
        s = end;
        if (i < n - 1) {
            while (*s == ' ') s++;
            if (*s != ',') return false;
            s++;
        }
    }
    while (*s == ' ') s++;
    return *s == '\0';
}

static void print_crop_help(void) {
    printf("Usage: tspdf crop <input.pdf> -o <output.pdf>\n");
    printf("             (--box x0,y0,x1,y1 | --margins t,r,b,l | --margin pt)\n");
    printf("             [--pages <range>]\n");
    printf("\nSet the CropBox of pages (the visible region). Content is kept,\n");
    printf("only the view is clipped. Choose ONE of:\n");
    printf("  --box x0,y0,x1,y1  explicit box in points, relative to the page's\n");
    printf("                     MediaBox origin (x1>x0, y1>y0)\n");
    printf("  --margins t,r,b,l  crop inward from the MediaBox by these margins\n");
    printf("  --margin pt        uniform inward margin on all four sides\n");
    printf("If --pages is omitted, all pages are cropped. The box is clamped to\n");
    printf("the MediaBox.\n");
}

int cmd_crop(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_crop_help();
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf crop: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf crop: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf crop: missing -o <output.pdf>\n");
        return 1;
    }

    const char *box_str = find_flag(argc, argv, "--box");
    const char *margins_str = find_flag(argc, argv, "--margins");
    const char *margin_str = find_flag(argc, argv, "--margin");

    int modes = (box_str != NULL) + (margins_str != NULL) + (margin_str != NULL);
    if (modes == 0) {
        fprintf(stderr, "tspdf crop: need one of --box, --margins, or --margin\n");
        return 1;
    }
    if (modes > 1) {
        fprintf(stderr, "tspdf crop: --box, --margins, and --margin are mutually exclusive\n");
        return 1;
    }

    double box[4] = {0};       // relative to MediaBox origin
    double margins[4] = {0};   // t, r, b, l
    if (box_str) {
        if (!parse_csv_doubles(box_str, box, 4)) {
            fprintf(stderr, "tspdf crop: invalid --box '%s' (want x0,y0,x1,y1)\n", box_str);
            return 1;
        }
        if (box[2] <= box[0] || box[3] <= box[1]) {
            fprintf(stderr, "tspdf crop: degenerate --box (need x1>x0 and y1>y0)\n");
            return 1;
        }
    } else if (margins_str) {
        if (!parse_csv_doubles(margins_str, margins, 4)) {
            fprintf(stderr, "tspdf crop: invalid --margins '%s' (want t,r,b,l)\n", margins_str);
            return 1;
        }
    } else {
        double m;
        if (!parse_csv_doubles(margin_str, &m, 1)) {
            fprintf(stderr, "tspdf crop: invalid --margin '%s'\n", margin_str);
            return 1;
        }
        margins[0] = margins[1] = margins[2] = margins[3] = m;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf crop: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    size_t total = tspdf_reader_page_count(doc);
    size_t page_count = 0;
    size_t *pages = NULL;
    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf crop: invalid page range '%s'\n", pages_spec);
            tspdf_reader_destroy(doc);
            return 1;
        }
        size_t bad = first_out_of_range(pages, page_count, total);
        if (bad != total) {
            fprintf(stderr, "tspdf crop: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, total, total == 1 ? "" : "s");
            free(pages);
            tspdf_reader_destroy(doc);
            return 1;
        }
    } else {
        pages = malloc((total ? total : 1) * sizeof(size_t));
        if (!pages) {
            fprintf(stderr, "tspdf crop: out of memory\n");
            tspdf_reader_destroy(doc);
            return 1;
        }
        for (size_t i = 0; i < total; i++) pages[i] = i;
        page_count = total;
    }

    // Compute an absolute CropBox per selected page from its own MediaBox.
    double *boxes = malloc((page_count ? page_count : 1) * 4 * sizeof(double));
    if (!boxes) {
        fprintf(stderr, "tspdf crop: out of memory\n");
        free(pages);
        tspdf_reader_destroy(doc);
        return 1;
    }
    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *pg = tspdf_reader_get_page(doc, pages[i]);
        double *b = &boxes[i * 4];
        const double *mb = pg->media_box;
        if (box_str) {
            b[0] = mb[0] + box[0];
            b[1] = mb[1] + box[1];
            b[2] = mb[0] + box[2];
            b[3] = mb[1] + box[3];
        } else {
            // margins: t, r, b, l inward from the MediaBox edges.
            b[0] = mb[0] + margins[3];        // left
            b[1] = mb[1] + margins[2];        // bottom
            b[2] = mb[2] - margins[1];        // right
            b[3] = mb[3] - margins[0];        // top
        }
        if (b[2] <= b[0] || b[3] <= b[1]) {
            fprintf(stderr, "tspdf crop: crop collapses page %zu to an empty box\n", pages[i] + 1);
            free(boxes);
            free(pages);
            tspdf_reader_destroy(doc);
            return 1;
        }
    }

    TspdfReader *result = tspdf_reader_set_cropboxes(doc, pages, page_count, boxes, &err);
    free(boxes);
    if (!result) {
        fprintf(stderr, "tspdf crop: crop failed: %s\n", tspdf_error_string(err));
        free(pages);
        tspdf_reader_destroy(doc);
        return 1;
    }

    err = tspdf_reader_save(result, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf crop: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    printf("Cropped %zu page(s) → %s\n", page_count, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pages);
    return 0;
}
