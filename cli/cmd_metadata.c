#include "commands.h"
#include "pipeline.h"
#include "../src/ops/ops.h"
#include "../include/tspdf.h"
#include "../src/util/pdfdate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_field(const char *label, const char *value) {
    if (value) printf("%-16s%s\n", label, value);
}

// Dates print human-readable ("2013-10-31 14:01:50 +04:00"); a value that
// is not a well-formed PDF date prints as-is.
static void print_date_field(const char *label, const char *value) {
    if (!value) return;
    char human[64];
    if (tspdf_format_pdf_date(value, human, sizeof(human)))
        printf("%-16s%s\n", label, human);
    else
        printf("%-16s%s\n", label, value);
}

static int run(TspdfCmdCtx *ctx) {
    int argc = ctx->argc;
    char **argv = ctx->argv;
    const char *input = ctx->input;
    const char *output = ctx->output;
    TspdfReader *doc = ctx->doc;
    TspdfError err = TSPDF_OK;

    // Collect --set and --clear values
    const char *sets[32];
    int nsets = find_flags(argc, argv, "--set", sets, 32);
    const char *clears[32];
    int nclears = find_flags(argc, argv, "--clear", clears, 32);

    if (nsets == 0 && nclears == 0) {
        // View mode
        printf("Metadata for %s:\n", input);
        print_field("Title:", tspdf_reader_get_title(doc));
        print_field("Author:", tspdf_reader_get_author(doc));
        print_field("Subject:", tspdf_reader_get_subject(doc));
        print_field("Keywords:", tspdf_reader_get_keywords(doc));
        print_field("Creator:", tspdf_reader_get_creator(doc));
        print_field("Producer:", tspdf_reader_get_producer(doc));
        print_date_field("Created:", tspdf_reader_get_creation_date(doc));
        print_date_field("Modified:", tspdf_reader_get_mod_date(doc));
        return 0;
    }

    // Edit mode
    if (!output) {
        fprintf(stderr, "tspdf metadata: --set/--clear requires -o <output.pdf>\n");
        return 1;
    }

    for (int i = 0; i < nsets; i++) {
        const char *eq = strchr(sets[i], '=');
        if (!eq) {
            fprintf(stderr, "tspdf metadata: invalid --set format '%s' (expected key=value)\n", sets[i]);
            return 1;
        }
        size_t key_len = (size_t)(eq - sets[i]);
        if (!tsops_metadata_set(doc, sets[i], key_len, eq + 1)) {
            fprintf(stderr, "tspdf metadata: unknown key '%.*s'\n", (int)key_len, sets[i]);
            return 1;
        }
    }

    // Clearing passes NULL through the setter: the serializer sees the
    // field as overridden-with-nothing and omits it from the Info dict.
    for (int i = 0; i < nclears; i++) {
        if (!tsops_metadata_set(doc, clears[i], strlen(clears[i]), NULL)) {
            fprintf(stderr, "tspdf metadata: unknown key '%s'\n", clears[i]);
            return 1;
        }
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf metadata: failed to save '%s': %s\n", output, tspdf_error_string(err));
        return 1;
    }

    printf("Updated %d field(s) → %s\n", nsets + nclears, output);

    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--set", true}, {"--clear", true},
    {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_metadata_spec = {
    .name = "metadata",
    .usage =
        "Usage: tspdf metadata <input.pdf>                                     # view\n"
        "       tspdf metadata <input.pdf> --set title=\"X\" --set author=\"Y\" -o out.pdf  # edit\n"
        "       tspdf metadata <input.pdf> --clear title -o out.pdf            # remove a field\n"
        "\nView or edit PDF metadata. --set and --clear are repeatable.\n"
        "Supported keys: title, author, subject, keywords, creator, producer\n"
        "Encrypted files: pass --password <pass> or --password-file <file>;\n"
        "edited output keeps the original encryption.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_TAKES_PASSWORD,
    .run = run,
};
