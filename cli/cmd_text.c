#include "commands.h"
#include "../include/tspdf.h"
#include "../src/reader/tspr_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Extract text, pdftotext-style: all pages to stdout by default, pages
// separated by form-feed (\f). Pages whose glyphs are mostly unmappable
// (CID fonts without /ToUnicode) get a stderr warning, never an in-text
// marker.
int cmd_text(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf text <input.pdf> [--layout] [--pages <range>] [--password <pass>] [-o <output.txt>]\n");
        printf("\nExtract text from a PDF (all pages to stdout by default).\n");
        printf("--layout preserves the page layout: columns and tables stay aligned.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf text: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf text: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];
    const char *output = find_flag(argc, argv, "-o");
    const char *password = find_flag(argc, argv, "--password");
    const char *pages_spec = find_flag(argc, argv, "--pages");
    bool layout = has_flag(argc, argv, "--layout");

    size_t page_count = 0;
    size_t *pages = NULL;
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf text: invalid page range '%s'\n", pages_spec);
            return 1;
        }
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf text: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(pages);
        return 1;
    }

    size_t total = tspdf_reader_page_count(doc);
    if (pages) {
        size_t bad = first_out_of_range(pages, page_count, total);
        if (bad != total) {
            fprintf(stderr, "tspdf text: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, total, total == 1 ? "" : "s");
            tspdf_reader_destroy(doc);
            free(pages);
            return 1;
        }
    }

    FILE *out = stdout;
    if (output) {
        out = fopen(output, "wb");
        if (!out) {
            fprintf(stderr, "tspdf text: failed to open '%s' for writing\n", output);
            tspdf_reader_destroy(doc);
            free(pages);
            return 1;
        }
    }

    size_t n = pages ? page_count : total;
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        size_t page_index = pages ? pages[i] : i;
        TspdfTextStats stats;
        const char *text = layout
            ? tspdf_reader_page_text_layout_stats(doc, page_index, &stats, &err)
            : tspdf_reader_page_text_stats(doc, page_index, &stats, &err);
        if (!text) {
            if (err == TSPDF_ERR_PAGE_RANGE) {
                fprintf(stderr, "tspdf text: page %zu is out of range (document has %zu page%s)\n",
                        page_index + 1, total, total == 1 ? "" : "s");
            } else {
                fprintf(stderr, "tspdf text: page %zu: extraction failed: %s\n",
                        page_index + 1, tspdf_error_string(err));
            }
            rc = 1;
            break;
        }
        if (i > 0) fputc('\f', out);
        fputs(text, out);
        if (stats.glyphs > 0 && stats.replacements * 2 > stats.glyphs) {
            fprintf(stderr, "tspdf text: warning: page %zu: most glyphs have no "
                    "Unicode mapping (font without /ToUnicode)\n", page_index + 1);
        }
    }

    if (out != stdout) fclose(out);
    tspdf_reader_destroy(doc);
    free(pages);
    return rc;
}
