#include "commands.h"
#include "pipeline.h"
#include "../src/ops/ops.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>

static int run(TspdfCmdCtx *ctx) {
    // Saves preserve a source document's encryption by default; decrypt is
    // the one command whose whole point is the opt-out.
    TspdfSaveOptions opts = tsops_unlock_save_options();
    TspdfError err = tspdf_reader_save_with_options(ctx->doc, ctx->output, &opts);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf decrypt: failed to save '%s': %s\n", ctx->output, tspdf_error_string(err));
        return 1;
    }

    printf("Decrypted → %s\n", ctx->output);
    return 0;
}

static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--password", true}, {"--password-file", true},
    {NULL, false}
};

const TspdfCmdSpec tspdf_cmd_decrypt_spec = {
    .name = "decrypt",
    .usage =
        "Usage: tspdf decrypt <input.pdf> -o <output.pdf> [--password <pass>]\n"
        "                     [--password-file <path>]\n"
        "\nDecrypt a password-protected PDF.\n"
        "The password can be passed with --password, read from the first line of a\n"
        "file with --password-file ('-' reads stdin), or typed at a hidden prompt\n"
        "when neither is given.\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_OPENS_INPUT | TSPDF_CMD_NEEDS_OUTPUT |
             TSPDF_CMD_TAKES_PASSWORD | TSPDF_CMD_PASSWORD_REQUIRED,
    .run = run,
};
