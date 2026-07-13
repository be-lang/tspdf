#include "commands.h"
#include "pipeline.h"
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

static int run(TspdfCmdCtx *ctx) {
    int argc = ctx->argc;
    char **argv = ctx->argv;
    const char *input = ctx->input;
    const char *output = ctx->output;
    TspdfReader *doc = ctx->doc;

    bool keep_metadata = has_flag(argc, argv, "--keep-metadata");
    bool lossy = has_flag(argc, argv, "--lossy");

    const char *dpi_arg = find_flag(argc, argv, "--image-dpi");
    const char *quality_arg = find_flag(argc, argv, "--image-quality");
    const char *mono_arg = find_flag(argc, argv, "--mono-dpi");
    if (!lossy && (dpi_arg || quality_arg || mono_arg)) {
        fprintf(stderr, "tspdf compress: %s requires --lossy\n",
                dpi_arg ? "--image-dpi" : quality_arg ? "--image-quality"
                                                      : "--mono-dpi");
        return 1;
    }

    int image_dpi = 150;
    int image_quality = 75;
    int mono_dpi = 300;
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
    if (mono_arg && (!parse_int_flag(mono_arg, &mono_dpi) ||
                     mono_dpi < 1 || mono_dpi > 4800)) {
        fprintf(stderr, "tspdf compress: invalid --mono-dpi '%s' (expected 1-4800)\n",
                mono_arg);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfLossyStats lossy_stats = {0};
    if (lossy) {
        err = tspdf_reader_lossy_images(doc, image_dpi, image_quality, mono_dpi,
                                        &lossy_stats);
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
        return 1;
    }

    if (lossy) {
        printf("Lossy: %zu image%s recompressed",
               lossy_stats.images_recompressed,
               lossy_stats.images_recompressed == 1 ? "" : "s");
        if (lossy_stats.images_mono > 0) {
            printf(" (%zu as CCITT G4)", lossy_stats.images_mono);
        }
        printf(" (%zu → %zu bytes), %zu skipped\n",
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

    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--image-dpi", true}, {"--image-quality", true},
    {"--mono-dpi", true}, {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_compress_spec = {
    .name = "compress",
    .usage =
        "Usage: tspdf compress <input.pdf> -o <output.pdf> [--keep-metadata]\n"
        "                      [--lossy] [--image-dpi N] [--image-quality N] [--mono-dpi N]\n"
        "\nStrip metadata and unused objects, recompress streams to reduce file size.\n"
        "Uncompressed streams shrink a lot; already well-compressed files only a little.\n"
        "By default removes all metadata (Info dict, XMP). Use --keep-metadata to preserve it.\n"
        "\n--lossy additionally downsamples oversized photos/scans and re-encodes them\n"
        "as JPEG, and black-and-white scans as CCITT G4. --image-dpi sets the target\n"
        "resolution for photos (default 150); --image-quality sets the JPEG quality\n"
        "1-100 (default 75); --mono-dpi sets the target for black-and-white images\n"
        "(default 300 - text needs more dpi than photos to stay legible).\n"
        "Without --lossy no image is altered.\n"
        "\nEncrypted files: pass --password <pass> or --password-file <file>;\n"
        "the output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
