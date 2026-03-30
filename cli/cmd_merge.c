#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_merge(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf merge <file1.pdf> <file2.pdf> [...] -o <output.pdf>\n");
        printf("\nMerge multiple PDF files into one.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf merge: missing -o <output.pdf>\n");
        return 1;
    }

    // Collect input files: positional args excluding the -o value
    const char *positional[64];
    int npos_raw = collect_positional(argc, argv, positional, 64);

    // Filter out the output path (it's positional but follows -o)
    const char *inputs[64];
    int npos = 0;
    for (int i = 0; i < npos_raw; i++) {
        if (positional[i] != output) {
            inputs[npos++] = positional[i];
        }
    }

    if (npos < 2) {
        fprintf(stderr, "tspdf merge: need at least 2 input files\n");
        return 1;
    }

    TspdfReader **docs = calloc((size_t)npos, sizeof(TspdfReader *));
    if (!docs) {
        fprintf(stderr, "tspdf merge: out of memory\n");
        return 1;
    }

    int ret = 0;
    TspdfError err = TSPDF_OK;

    for (int i = 0; i < npos; i++) {
        docs[i] = tspdf_reader_open_file(inputs[i], &err);
        if (!docs[i]) {
            fprintf(stderr, "tspdf merge: failed to open '%s': %s\n",
                    inputs[i], tspdf_error_string(err));
            ret = 1;
            goto cleanup;
        }
    }

    TspdfReader *merged = tspdf_reader_merge(docs, (size_t)npos, &err);
    if (!merged) {
        fprintf(stderr, "tspdf merge: merge failed: %s\n", tspdf_error_string(err));
        ret = 1;
        goto cleanup;
    }

    err = tspdf_reader_save(merged, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf merge: failed to save '%s': %s\n", output, tspdf_error_string(err));
        ret = 1;
    } else {
        printf("Merged %d files → %s\n", npos, output);
    }

    tspdf_reader_destroy(merged);

cleanup:
    for (int i = 0; i < npos; i++) {
        if (docs[i]) tspdf_reader_destroy(docs[i]);
    }
    free(docs);
    return ret;
}
