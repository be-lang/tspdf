#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Parse a decimal integer flag value; false on garbage or trailing junk.
static bool parse_int_flag(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < -2147483647L || v > 2147483647L) return false;
    *out = (int)v;
    return true;
}

int cmd_compress(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf compress <input.pdf> -o <output.pdf> [--keep-metadata]\n");
        printf("                      [--lossy] [--image-dpi N] [--image-quality N]\n");
        printf("\nStrip metadata and unused objects, recompress streams to reduce file size.\n");
        printf("Uncompressed streams shrink a lot; already well-compressed files only a little.\n");
        printf("By default removes all metadata (Info dict, XMP). Use --keep-metadata to preserve it.\n");
        printf("\n--lossy additionally downsamples oversized photos/scans and re-encodes them\n");
        printf("as JPEG. --image-dpi sets the target resolution (default 150); --image-quality\n");
        printf("sets the JPEG quality 1-100 (default 75). Without --lossy no image is altered.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf compress: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf compress: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf compress: missing -o <output.pdf>\n");
        return 1;
    }

    bool keep_metadata = has_flag(argc, argv, "--keep-metadata");
    bool lossy = has_flag(argc, argv, "--lossy");

    const char *dpi_arg = find_flag(argc, argv, "--image-dpi");
    const char *quality_arg = find_flag(argc, argv, "--image-quality");
    if (!lossy && (dpi_arg || quality_arg)) {
        fprintf(stderr, "tspdf compress: %s requires --lossy\n",
                dpi_arg ? "--image-dpi" : "--image-quality");
        return 1;
    }

    int image_dpi = 150;
    int image_quality = 75;
    if (dpi_arg && (!parse_int_flag(dpi_arg, &image_dpi) ||
                    image_dpi < 1 || image_dpi > 4800)) {
        fprintf(stderr, "tspdf compress: invalid --image-dpi '%s' (expected 1-4800)\n",
                dpi_arg);
        return 1;
    }
    if (quality_arg && (!parse_int_flag(quality_arg, &image_quality) ||
                        image_quality < 1 || image_quality > 100)) {
        fprintf(stderr, "tspdf compress: invalid --image-quality '%s' (expected 1-100)\n",
                quality_arg);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf compress: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    TspdfLossyStats lossy_stats = {0};
    if (lossy) {
        err = tspdf_reader_lossy_images(doc, image_dpi, image_quality, &lossy_stats);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf compress: lossy image pass failed: %s\n",
                    tspdf_error_string(err));
            tspdf_reader_destroy(doc);
            return 1;
        }
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

    if (lossy) {
        printf("Lossy: %zu image%s recompressed (%zu → %zu bytes), %zu skipped\n",
               lossy_stats.images_recompressed,
               lossy_stats.images_recompressed == 1 ? "" : "s",
               lossy_stats.bytes_before, lossy_stats.bytes_after,
               lossy_stats.images_skipped);
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
