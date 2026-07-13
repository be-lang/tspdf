#include "commands.h"
#include "pipeline.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// "<n>" zero-padded to `width` digits (clamped to what a size_t needs).
static void zero_pad(char *dst, size_t dst_sz, size_t n, int width) {
    char digits[24];
    int nd = 0;
    do { digits[nd++] = (char)('0' + n % 10); n /= 10; } while (n > 0);
    size_t out = 0;
    for (int pad = width - nd; pad > 0 && out + 1 < dst_sz; pad--) dst[out++] = '0';
    while (nd > 0 && out + 1 < dst_sz) dst[out++] = digits[--nd];
    dst[out] = '\0';
}

// Drop every document-level embedded file from `doc` (for --no-attachments;
// extract copies attachments into each part by default). Returns TSPDF_OK
// when there was nothing to drop.
static TspdfError split_drop_attachments(TspdfReader *doc) {
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_attachments(doc, &infos, &count);
    for (size_t i = 0; i < count && err == TSPDF_OK; i++) {
        err = tspdf_reader_attachment_remove(doc, infos[i].name);
    }
    return err;
}

// Burst mode: write every page of `doc` to its own file. `output` is the
// name template: "out.pdf" becomes out-001.pdf, out-002.pdf, ... zero-padded
// to the page-count width (minimum 3 digits).
static int split_burst(TspdfReader *doc, const char *output, bool no_attachments) {
    size_t total = tspdf_reader_page_count(doc);
    if (total == 0) {
        fprintf(stderr, "tspdf split: document has no pages\n");
        return 1;
    }

    // Base name = output minus a trailing ".pdf" (case-insensitive).
    size_t base_len = strlen(output);
    if (base_len >= 4 && strcasecmp(output + base_len - 4, ".pdf") == 0)
        base_len -= 4;

    int width = 1;
    for (size_t t = total; t >= 10; t /= 10) width++;
    if (width < 3) width = 3;

    char *name = malloc(base_len + 32);
    char *first_name = malloc(base_len + 32);
    if (!name || !first_name) {
        fprintf(stderr, "tspdf split: out of memory\n");
        free(name);
        free(first_name);
        return 1;
    }

    for (size_t i = 0; i < total; i++) {
        char num[24];
        zero_pad(num, sizeof(num), i + 1, width);
        memcpy(name, output, base_len);
        snprintf(name + base_len, 32, "-%s.pdf", num);
        if (i == 0) memcpy(first_name, name, strlen(name) + 1);

        TspdfError err = TSPDF_OK;
        TspdfReader *page = tspdf_reader_extract(doc, &i, 1, &err);
        if (!page) {
            fprintf(stderr, "tspdf split: extract of page %zu failed: %s\n",
                    i + 1, tspdf_error_string(err));
            free(name);
            free(first_name);
            return 1;
        }
        if (no_attachments && (err = split_drop_attachments(page)) != TSPDF_OK) {
            fprintf(stderr, "tspdf split: dropping attachments failed: %s\n",
                    tspdf_error_string(err));
            tspdf_reader_destroy(page);
            free(name);
            free(first_name);
            return 1;
        }

        // Only write objects reachable from this page (same rationale as the
        // strip_unused_objects note in the --pages path below).
        TspdfSaveOptions opts = tspdf_save_options_default();
        opts.strip_unused_objects = true;
        err = tspdf_reader_save_with_options(page, name, &opts);
        tspdf_reader_destroy(page);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf split: failed to save '%s': %s\n",
                    name, tspdf_error_string(err));
            free(name);
            free(first_name);
            return 1;
        }
    }

    printf("Split %zu page(s) → %s .. %s\n", total, first_name, name);
    free(name);
    free(first_name);
    return 0;
}

// First 0-based page index in `pages` that is out of range for `total`.
static int run(TspdfCmdCtx *ctx) {
    int argc = ctx->argc;
    char **argv = ctx->argv;
    const char *output = ctx->output;
    TspdfReader *doc = ctx->doc;
    TspdfError err = TSPDF_OK;

    const char *pages_spec = find_flag(argc, argv, "--pages");
    bool no_attachments = has_flag(argc, argv, "--no-attachments");
    if (has_flag(argc, argv, "--burst") && pages_spec) {
        fprintf(stderr, "tspdf split: --burst and --pages are mutually exclusive\n");
        return 1;
    }

    if (!pages_spec) {
        // Per-page split (burst) is the default when no page range is given.
        return split_burst(doc, output, no_attachments);
    }

    size_t page_count;
    size_t *pages = parse_page_range(pages_spec, &page_count);
    if (!pages) {
        fprintf(stderr, "tspdf split: invalid page range '%s'\n", pages_spec);
        return 1;
    }

    TspdfReader *result = tspdf_reader_extract(doc, pages, page_count, &err);
    if (!result) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            size_t total = tspdf_reader_page_count(doc);
            fprintf(stderr, "tspdf split: page %zu is out of range (document has %zu page%s)\n",
                    first_out_of_range(pages, page_count, total) + 1, total,
                    total == 1 ? "" : "s");
        } else {
            fprintf(stderr, "tspdf split: extract failed: %s\n", tspdf_error_string(err));
        }
        free(pages);
        return 1;
    }

    if (no_attachments) {
        err = split_drop_attachments(result);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf split: dropping attachments failed: %s\n",
                    tspdf_error_string(err));
            tspdf_reader_destroy(result);
            free(pages);
            return 1;
        }
    }

    // Only write objects reachable from the extracted pages; without this the
    // output would carry every object of the source document along.
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    err = tspdf_reader_save_with_options(result, output, &opts);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf split: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        free(pages);
        return 1;
    }

    printf("Extracted %zu page(s) → %s\n", page_count, output);

    tspdf_reader_destroy(result);
    free(pages);
    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--pages", true},
    {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_split_spec = {
    .name = "split",
    .usage =
        "Usage: tspdf split <input.pdf> [--pages 1-3,5 | --burst] [--no-attachments] -o <output.pdf>\n"
        "\nExtract specific pages from a PDF, or split it into one file per page.\n"
        "With --pages, the selected pages are written to the output file.\n"
        "With --burst (the default when --pages is absent), each page is written\n"
        "to its own file: out.pdf becomes out-001.pdf, out-002.pdf, ...\n"
        "Embedded file attachments are copied into every output; --no-attachments\n"
        "drops them instead.\n"
        "Encrypted files: pass --password <pass> or --password-file <file>;\n"
        "every output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
