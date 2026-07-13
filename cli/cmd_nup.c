// tspdf nup: N-up page imposition — place multiple source pages onto each
// output sheet (2-up, 4-up, ...), the classic pdfjam/pdftk imposition feature.
// The heavy lifting lives in the reader (tspdf_reader_nup): each selected page
// is imported as a self-contained /Form XObject and drawn into its grid cell,
// scaled to fit with the aspect ratio preserved.

#include "commands.h"
#include "pipeline.h"
#include "../include/tspdf_overlay.h"
#include "password_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_nup_help(void) {
    printf("Usage: tspdf nup <N> <input.pdf> -o <output.pdf>\n");
    printf("                 [--pages <range>] [--page-size a4|letter|source]\n");
    printf("                 [--landscape] [--gap <pt>] [--frame] [--password <pass>]\n");
    printf("\nPlace N source pages onto each output sheet in a grid (imposition).\n");
    printf("N is one of 2, 4, 6, 8, 9, 16. Each page is scaled to fit its cell,\n");
    printf("aspect ratio preserved and centered, in reading order.\n");
}

int cmd_nup(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_nup_help();
        return argc == 0 ? 1 : 0;
    }

    // Positionals: <N> <input.pdf>
    const char *positional[3];
    int npos = collect_positional(argc, argv, positional, 3);
    if (npos < 2) {
        fprintf(stderr, "tspdf nup: usage: tspdf nup <N> <input.pdf> -o <output.pdf>\n");
        return 1;
    }
    if (npos > 2) {
        fprintf(stderr, "tspdf nup: unexpected extra argument '%s'\n", positional[2]);
        return 1;
    }

    char *end = NULL;
    long n = strtol(positional[0], &end, 10);
    if (end == positional[0] || *end != '\0' || n < 2 || n > 16) {
        fprintf(stderr, "tspdf nup: N must be one of 2, 4, 6, 8, 9, 16 (got '%s')\n",
                positional[0]);
        return 1;
    }
    switch (n) {
        case 2: case 4: case 6: case 8: case 9: case 16: break;
        default:
            fprintf(stderr, "tspdf nup: N must be one of 2, 4, 6, 8, 9, 16 (got %ld)\n", n);
            return 1;
    }
    const char *input = positional[1];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf nup: missing -o <output.pdf>\n");
        return 1;
    }

    TspdfNupPageSize size = TSPDF_NUP_SIZE_A4;
    const char *ps = find_flag(argc, argv, "--page-size");
    if (ps) {
        if (strcmp(ps, "a4") == 0)          size = TSPDF_NUP_SIZE_A4;
        else if (strcmp(ps, "letter") == 0) size = TSPDF_NUP_SIZE_LETTER;
        else if (strcmp(ps, "source") == 0) size = TSPDF_NUP_SIZE_SOURCE;
        else {
            fprintf(stderr, "tspdf nup: unknown --page-size '%s' (use a4, letter, or source)\n", ps);
            return 1;
        }
    }

    bool landscape = has_flag(argc, argv, "--landscape");
    bool frame = has_flag(argc, argv, "--frame");

    double gap = 0.0;
    const char *gap_str = find_flag(argc, argv, "--gap");
    if (gap_str) {
        char *gend = NULL;
        gap = strtod(gap_str, &gend);
        if (gend == gap_str || *gend != '\0' || !(gap >= 0.0)) {
            fprintf(stderr, "tspdf nup: invalid --gap '%s' (points, >= 0)\n", gap_str);
            return 1;
        }
    }

    size_t sel_count = 0;
    size_t *sel = NULL;
    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        sel = parse_page_range(pages_spec, &sel_count);
        if (!sel) {
            fprintf(stderr, "tspdf nup: invalid page range '%s'\n", pages_spec);
            return 1;
        }
    }

    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "nup", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf nup: '%s' is encrypted; use --password or --password-file\n", input);
        } else {
            fprintf(stderr, "tspdf nup: failed to open '%s': %s\n", input, tspdf_error_string(err));
        }
        free(sel);
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);
    if (sel) {
        size_t bad = first_out_of_range(sel, sel_count, page_count);
        if (bad != page_count) {
            fprintf(stderr, "tspdf nup: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, page_count, page_count == 1 ? "" : "s");
            tspdf_reader_destroy(doc);
            free(sel);
            return 1;
        }
    }

    TspdfNupOptions opts = {0};
    opts.n = (unsigned)n;
    opts.pages = sel;
    opts.page_count = sel_count;
    opts.size = size;
    opts.landscape = landscape;
    opts.gap = gap;
    opts.frame = frame;

    TspdfReader *out = tspdf_reader_nup(doc, &opts, &err);
    if (!out) {
        fprintf(stderr, "tspdf nup: imposition failed: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        free(sel);
        return 1;
    }

    err = tspdf_reader_save(out, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf nup: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(out);
        tspdf_reader_destroy(doc);
        free(sel);
        return 1;
    }

    size_t sheets = tspdf_reader_page_count(out);
    size_t placed = sel ? sel_count : page_count;
    printf("Imposed %zu page(s) %ld-up onto %zu sheet(s) → %s\n", placed, n, sheets, output);

    tspdf_reader_destroy(out);
    tspdf_reader_destroy(doc);
    free(sel);
    return 0;
}


/* nup takes two positionals (<N> <input.pdf>) with a custom usage message — RAW_ARGS. */
static int run_nup_raw(TspdfCmdCtx *ctx) { return cmd_nup(ctx->argc, ctx->argv); }
static const TspdfCliFlag NUP_FLAGS[] = {
    {"-o", true},
    {"--page-size", true},
    {"--gap", true},
    {"--pages", true},
    {"--password", true},
    {"--password-file", true},
    {NULL, false}
};
const TspdfCmdSpec tspdf_cmd_nup_spec = {
    .name = "nup",
    .flags = NUP_FLAGS,
    .min_pos = 0, .max_pos = -1,
    .needs = TSPDF_CMD_RAW_ARGS,
    .run = run_nup_raw,
};
