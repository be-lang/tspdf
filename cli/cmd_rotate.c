#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_rotate(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf rotate <input.pdf> [--pages 1,3] --angle 90 -o <output.pdf>\n");
        printf("\nRotate pages in a PDF. Angle must be 90, 180, or 270.\n");
        printf("If --pages is omitted, all pages are rotated.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf rotate: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf rotate: missing -o <output.pdf>\n");
        return 1;
    }

    const char *angle_str = find_flag(argc, argv, "--angle");
    if (!angle_str) {
        fprintf(stderr, "tspdf rotate: missing --angle <90|180|270>\n");
        return 1;
    }
    int angle = atoi(angle_str);
    if (angle != 90 && angle != 180 && angle != 270) {
        fprintf(stderr, "tspdf rotate: angle must be 90, 180, or 270 (got %d)\n", angle);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf rotate: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    size_t page_count = 0;
    size_t *pages = NULL;

    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf rotate: invalid page range '%s'\n", pages_spec);
            tspdf_reader_destroy(doc);
            return 1;
        }
    } else {
        // All pages
        size_t total = tspdf_reader_page_count(doc);
        pages = malloc(total * sizeof(size_t));
        if (!pages) {
            fprintf(stderr, "tspdf rotate: out of memory\n");
            tspdf_reader_destroy(doc);
            return 1;
        }
        for (size_t i = 0; i < total; i++) pages[i] = i;
        page_count = total;
    }

    TspdfReader *result = tspdf_reader_rotate(doc, pages, page_count, angle, &err);
    if (!result) {
        fprintf(stderr, "tspdf rotate: rotate failed: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    err = tspdf_reader_save(result, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf rotate: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    printf("Rotated %zu page(s) by %d° → %s\n", page_count, angle, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pages);
    return 0;
}
