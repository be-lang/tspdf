#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_split(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf split <input.pdf> --pages 1-3,5 -o <output.pdf>\n");
        printf("\nExtract specific pages from a PDF.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf split: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf split: missing -o <output.pdf>\n");
        return 1;
    }

    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (!pages_spec) {
        fprintf(stderr, "tspdf split: missing --pages <range>\n");
        return 1;
    }

    size_t page_count;
    size_t *pages = parse_page_range(pages_spec, &page_count);
    if (!pages) {
        fprintf(stderr, "tspdf split: invalid page range '%s'\n", pages_spec);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf split: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(pages);
        return 1;
    }

    TspdfReader *result = tspdf_reader_extract(doc, pages, page_count, &err);
    if (!result) {
        fprintf(stderr, "tspdf split: extract failed: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    err = tspdf_reader_save(result, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf split: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    printf("Extracted %zu page(s) → %s\n", page_count, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pages);
    return 0;
}
