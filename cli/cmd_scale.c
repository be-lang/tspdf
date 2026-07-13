// tspdf scale: resize pages, scaling their content (not clipping). Either a
// uniform --factor (grow/shrink page + content) or --to a named page size
// (fit content into that size, aspect preserved, centered).

#include "commands.h"
#include "pipeline.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Named page sizes in PDF points (1/72").
static bool named_size(const char *name, double *w, double *h) {
    if (!name) return false;
    if (strcasecmp(name, "a4") == 0)     { *w = 595.28; *h = 841.89; return true; }
    if (strcasecmp(name, "letter") == 0) { *w = 612.0;  *h = 792.0;  return true; }
    if (strcasecmp(name, "legal") == 0)  { *w = 612.0;  *h = 1008.0; return true; }
    if (strcasecmp(name, "a3") == 0)     { *w = 841.89; *h = 1190.55; return true; }
    if (strcasecmp(name, "a5") == 0)     { *w = 419.53; *h = 595.28; return true; }
    return false;
}

static int run(TspdfCmdCtx *ctx) {
    TspdfReader *doc = ctx->doc;
    TspdfError err = TSPDF_OK;

    const char *factor_str = find_flag(ctx->argc, ctx->argv, "--factor");
    const char *to_str = find_flag(ctx->argc, ctx->argv, "--to");
    if ((factor_str != NULL) + (to_str != NULL) != 1) {
        fprintf(stderr, "tspdf scale: need exactly one of --factor or --to\n");
        return 1;
    }

    double factor = 0.0, tw = 0.0, th = 0.0;
    if (factor_str) {
        char *end = NULL;
        factor = strtod(factor_str, &end);
        if (end == factor_str || *end != '\0' || !(factor > 0.0)) {
            fprintf(stderr, "tspdf scale: invalid --factor '%s' (need > 0)\n", factor_str);
            return 1;
        }
    } else {
        if (!named_size(to_str, &tw, &th)) {
            fprintf(stderr, "tspdf scale: unknown --to size '%s' (a4, letter, legal, a3, a5)\n", to_str);
            return 1;
        }
    }

    size_t total = tspdf_reader_page_count(doc);
    size_t page_count = 0;
    size_t *pages = NULL;
    const char *pages_spec = find_flag(ctx->argc, ctx->argv, "--pages");
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf scale: invalid page range '%s'\n", pages_spec);
            return 1;
        }
        size_t bad = first_out_of_range(pages, page_count, total);
        if (bad != total) {
            fprintf(stderr, "tspdf scale: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, total, total == 1 ? "" : "s");
            free(pages);
            return 1;
        }
    } else {
        pages = malloc((total ? total : 1) * sizeof(size_t));
        if (!pages) {
            fprintf(stderr, "tspdf scale: out of memory\n");
            return 1;
        }
        for (size_t i = 0; i < total; i++) pages[i] = i;
        page_count = total;
    }

    TspdfReader *result = factor_str
        ? tspdf_reader_scale(doc, pages, page_count, factor, factor, &err)
        : tspdf_reader_resize_to(doc, pages, page_count, tw, th, &err);
    if (!result) {
        fprintf(stderr, "tspdf scale: scale failed: %s\n", tspdf_error_string(err));
        free(pages);
        return 1;
    }

    err = tspdf_reader_save(result, ctx->output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf scale: failed to save '%s': %s\n", ctx->output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        free(pages);
        return 1;
    }

    if (factor_str)
        printf("Scaled %zu page(s) by %.4g → %s\n", page_count, factor, ctx->output);
    else
        printf("Resized %zu page(s) to %s → %s\n", page_count, to_str, ctx->output);

    tspdf_reader_destroy(result);
    free(pages);
    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--factor", true}, {"--to", true}, {"--pages", true},
    {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_scale_spec = {
    .name = "scale",
    .usage =
        "Usage: tspdf scale <input.pdf> -o <output.pdf>\n"
        "             (--factor f | --to a4|letter|legal|a3|a5)\n"
        "             [--pages <range>]\n"
        "\nResize pages, scaling the content with them (not clipping). One of:\n"
        "  --factor f   uniform scale of page dimensions and content (f>0)\n"
        "  --to SIZE    fit content into the named page size, aspect preserved\n"
        "               and centered; the MediaBox becomes that size\n"
        "If --pages is omitted, all pages are scaled.\n"
        "Encrypted files: pass --password <pass> or --password-file <file>;\n"
        "the output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
