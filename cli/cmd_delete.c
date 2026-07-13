#include "commands.h"
#include "pipeline.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run(TspdfCmdCtx *ctx) {
    TspdfReader *doc = ctx->doc;
    TspdfError err = TSPDF_OK;

    const char *pages_spec = find_flag(ctx->argc, ctx->argv, "--pages");
    if (!pages_spec) {
        fprintf(stderr, "tspdf delete: missing --pages <range>\n");
        return 1;
    }

    size_t page_count;
    size_t *pages = parse_page_range(pages_spec, &page_count);
    if (!pages) {
        fprintf(stderr, "tspdf delete: invalid page range '%s'\n", pages_spec);
        return 1;
    }

    TspdfReader *result = tspdf_reader_delete(doc, pages, page_count, &err);
    if (!result) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            size_t total = tspdf_reader_page_count(doc);
            fprintf(stderr, "tspdf delete: page %zu is out of range (document has %zu page%s)\n",
                    first_out_of_range(pages, page_count, total) + 1, total,
                    total == 1 ? "" : "s");
        } else {
            fprintf(stderr, "tspdf delete: delete failed: %s\n", tspdf_error_string(err));
        }
        free(pages);
        return 1;
    }

    err = tspdf_reader_save(result, ctx->output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf delete: failed to save '%s': %s\n", ctx->output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        free(pages);
        return 1;
    }

    printf("Deleted %zu page(s) → %s\n", page_count, ctx->output);

    tspdf_reader_destroy(result);
    free(pages);
    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--pages", true},
    {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_delete_spec = {
    .name = "delete",
    .usage =
        "Usage: tspdf delete <input.pdf> --pages 2,4 -o <output.pdf>\n"
        "\nDelete specific pages from a PDF.\n"
        "Encrypted files: pass --password <pass> or --password-file <file>;\n"
        "the output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
