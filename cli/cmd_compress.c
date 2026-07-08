#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int cmd_compress(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf compress <input.pdf> -o <output.pdf> [--keep-metadata]\n");
        printf("\nStrip metadata and unused objects, recompress streams to reduce file size.\n");
        printf("By default removes all metadata (Info dict, XMP). Use --keep-metadata to preserve it.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf compress: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf compress: missing -o <output.pdf>\n");
        return 1;
    }

    bool keep_metadata = has_flag(argc, argv, "--keep-metadata");

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf compress: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    opts.strip_metadata = !keep_metadata;

    err = tspdf_reader_save_with_options(doc, output, &opts);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf compress: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    struct stat st_in, st_out;
    if (stat(input, &st_in) == 0 && stat(output, &st_out) == 0) {
        double pct = st_in.st_size > 0
            ? (1.0 - (double)st_out.st_size / (double)st_in.st_size) * 100.0
            : 0.0;
        printf("Compressed → %s (%ld → %ld bytes, %.1f%% reduction)\n",
               output, (long)st_in.st_size, (long)st_out.st_size, pct);
    } else {
        printf("Compressed → %s\n", output);
    }

    tspdf_reader_destroy(doc);
    return 0;
}
