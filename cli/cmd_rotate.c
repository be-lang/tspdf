#include "commands.h"
#include "pipeline.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(TspdfCmdCtx *ctx) {
    TspdfReader *doc = ctx->doc;
    TspdfError err = TSPDF_OK;

    const char *angle_str = find_flag(ctx->argc, ctx->argv, "--angle");
    if (!angle_str) {
        fprintf(stderr, "tspdf rotate: missing --angle <90|180|270>\n");
        return 1;
    }
    int angle = atoi(angle_str);
    if (angle != 90 && angle != 180 && angle != 270) {
        fprintf(stderr, "tspdf rotate: angle must be 90, 180, or 270 (got %d)\n", angle);
        return 1;
    }

    size_t page_count = 0;
    size_t *pages = NULL;

    const char *pages_spec = find_flag(ctx->argc, ctx->argv, "--pages");
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf rotate: invalid page range '%s'\n", pages_spec);
            return 1;
        }
    } else {
        // All pages
        size_t total = tspdf_reader_page_count(doc);
        pages = malloc(total * sizeof(size_t));
        if (!pages) {
            fprintf(stderr, "tspdf rotate: out of memory\n");
            return 1;
        }
        for (size_t i = 0; i < total; i++) pages[i] = i;
        page_count = total;
    }

    TspdfReader *result = tspdf_reader_rotate(doc, pages, page_count, angle, &err);
    if (!result) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            size_t total = tspdf_reader_page_count(doc);
            fprintf(stderr, "tspdf rotate: page %zu is out of range (document has %zu page%s)\n",
                    first_out_of_range(pages, page_count, total) + 1, total,
                    total == 1 ? "" : "s");
        } else {
            fprintf(stderr, "tspdf rotate: rotate failed: %s\n", tspdf_error_string(err));
        }
        free(pages);
        return 1;
    }

    err = tspdf_reader_save(result, ctx->output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf rotate: failed to save '%s': %s\n", ctx->output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        free(pages);
        return 1;
    }

    printf("Rotated %zu page(s) by %d° → %s\n", page_count, angle, ctx->output);

    tspdf_reader_destroy(result);
    free(pages);
    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--pages", true}, {"--angle", true},
    {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_rotate_spec = {
    .name = "rotate",
    .usage =
        "Usage: tspdf rotate <input.pdf> [--pages 1,3] --angle 90 -o <output.pdf>\n"
        "\nRotate pages in a PDF. Angle must be 90, 180, or 270.\n"
        "If --pages is omitted, all pages are rotated.\n"
        "Encrypted files: pass --password <pass> or --password-file <file>;\n"
        "the output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
