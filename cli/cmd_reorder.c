#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int cmd_reorder(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf reorder <input.pdf> --order 3,1,2 -o <output.pdf>\n");
        printf("\nReorder pages in a PDF. Order must list all pages exactly once.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf reorder: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf reorder: missing -o <output.pdf>\n");
        return 1;
    }

    const char *order_spec = find_flag(argc, argv, "--order");
    if (!order_spec) {
        fprintf(stderr, "tspdf reorder: missing --order <page,page,...>\n");
        return 1;
    }

    // Parse comma-separated 1-based page numbers
    size_t capacity = 16;
    size_t count = 0;
    size_t *order = malloc(capacity * sizeof(size_t));
    if (!order) {
        fprintf(stderr, "tspdf reorder: out of memory\n");
        return 1;
    }

    const char *p = order_spec;
    while (*p) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        if (!isdigit((unsigned char)*p)) {
            fprintf(stderr, "tspdf reorder: invalid order spec '%s'\n", order_spec);
            free(order);
            return 1;
        }
        char *end;
        long val = strtol(p, &end, 10);
        if (val < 1) {
            fprintf(stderr, "tspdf reorder: page numbers must be >= 1\n");
            free(order);
            return 1;
        }
        if (count >= capacity) {
            capacity *= 2;
            size_t *tmp = realloc(order, capacity * sizeof(size_t));
            if (!tmp) { free(order); return 1; }
            order = tmp;
        }
        order[count++] = (size_t)(val - 1);  // Convert to 0-based
        p = end;
        while (*p == ' ') p++;
        if (*p == ',') p++;
        else if (*p != '\0') {
            fprintf(stderr, "tspdf reorder: invalid order spec '%s'\n", order_spec);
            free(order);
            return 1;
        }
    }

    if (count == 0) {
        fprintf(stderr, "tspdf reorder: empty order spec\n");
        free(order);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf reorder: failed to open '%s': %s\n", input, tspdf_error_string(err));
        free(order);
        return 1;
    }

    size_t total = tspdf_reader_page_count(doc);
    if (count != total) {
        fprintf(stderr, "tspdf reorder: order has %zu entries but document has %zu pages\n", count, total);
        tspdf_reader_destroy(doc);
        free(order);
        return 1;
    }

    TspdfReader *result = tspdf_reader_reorder(doc, order, count, &err);
    if (!result) {
        fprintf(stderr, "tspdf reorder: reorder failed: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(order);
        return 1;
    }

    err = tspdf_reader_save(result, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf reorder: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(order);
        return 1;
    }

    printf("Reordered %zu pages → %s\n", count, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(order);
    return 0;
}
