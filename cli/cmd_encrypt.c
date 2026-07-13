#include "commands.h"
#include "pipeline.h"
#include "password_input.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// User-access permission bits, ISO 32000-1 Table 22 (bit positions are
// 1-based in the spec: bit 3 = print, ..., bit 12 = high-quality print).
static const struct {
    const char *name;
    uint32_t bit;
} PERMISSIONS[] = {
    {"print",    1u << 2},   // bit 3: print the document
    {"modify",   1u << 3},   // bit 4: modify contents
    {"copy",     1u << 4},   // bit 5: copy text and graphics
    {"annotate", 1u << 5},   // bit 6: add/modify annotations
    {"forms",    1u << 8},   // bit 9: fill in form fields
    {"extract",  1u << 9},   // bit 10: extract for accessibility
    {"assemble", 1u << 10},  // bit 11: insert/rotate/delete pages
    {"print-hq", 1u << 11},  // bit 12: high-resolution printing
};

// Parse "print,copy,..." into a /P value. Listed actions are allowed;
// everything else is denied. Reserved bits are set per the spec: bits 7-8
// and 13-32 must be 1, bits 1-2 must be 0. Returns false on unknown tokens.
static bool parse_permissions(const char *spec, uint32_t *out) {
    uint32_t p = 0xFFFFF0C0u;
    const char *tok = spec;
    while (*tok) {
        const char *end = strchr(tok, ',');
        size_t len = end ? (size_t)(end - tok) : strlen(tok);
        bool found = false;
        for (size_t i = 0; i < sizeof(PERMISSIONS) / sizeof(PERMISSIONS[0]); i++) {
            if (strlen(PERMISSIONS[i].name) == len &&
                strncmp(tok, PERMISSIONS[i].name, len) == 0) {
                p |= PERMISSIONS[i].bit;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "tspdf encrypt: unknown permission '%.*s' (valid: "
                            "print, modify, copy, annotate, forms, extract, "
                            "assemble, print-hq)\n", (int)len, tok);
            return false;
        }
        if (!end) break;
        tok = end + 1;
    }
    // High-quality print (bit 12) is meaningless without plain print (bit 3):
    // a viewer that honors /P would deny printing entirely, so "print-hq"
    // alone silently blocks all printing. Auto-enable print when print-hq is
    // requested (friendlier than erroring).
    if (p & (1u << 11)) p |= (1u << 2);
    *out = p;
    return true;
}

static int run(TspdfCmdCtx *ctx) {
    int argc = ctx->argc;
    char **argv = ctx->argv;
    const char *input = ctx->input;
    const char *output = ctx->output;
    const char *password = ctx->password;  // resolved & required by the pipeline

    // Owner password is optional: no prompt, default to the user password.
    char owner_buf[TSPDF_PASSWORD_MAX];
    bool owner_given = find_flag(argc, argv, "--owner-password") != NULL ||
                       find_flag(argc, argv, "--owner-password-file") != NULL;
    const char *owner_pass = tspdf_resolve_password(argc, argv,
                                                    "--owner-password", "--owner-password-file",
                                                    "encrypt", "Owner password: ",
                                                    false, owner_buf, sizeof(owner_buf));
    if (!owner_pass) {
        if (owner_given) return 1;  // flag given but the file was unreadable
        owner_pass = password;
    }

    const char *bits_str = find_flag(argc, argv, "--bits");
    int bits = 128;
    if (bits_str) {
        bits = atoi(bits_str);
        if (bits != 128 && bits != 256) {
            fprintf(stderr, "tspdf encrypt: --bits must be 128 or 256 (got %d)\n", bits);
            return 1;
        }
    }

    // Without --permissions everything stays allowed (the historical
    // default). An empty list denies every controllable action.
    uint32_t permissions = 0xFFFFFFFFu;
    const char *perm_spec = find_flag(argc, argv, "--permissions");
    if (perm_spec && !parse_permissions(perm_spec, &permissions)) {
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf encrypt: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    err = tspdf_reader_save_encrypted(doc, output, password, owner_pass, permissions, bits);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf encrypt: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("Encrypted (AES-%d) → %s\n", bits, output);

    tspdf_reader_destroy(doc);
    return 0;
}


static const TspdfCliFlag FLAGS[] = {
    {"-o", true}, {"--password", true}, {"--password-file", true},
    {"--owner-password", true}, {"--owner-password-file", true},
    {"--bits", true}, {"--permissions", true},
    {NULL, false}
};

// encrypt opens the *plaintext* input itself (no password on open), so it does
// not set OPENS_INPUT; the pipeline still resolves & requires the user password.
const TspdfCmdSpec tspdf_cmd_encrypt_spec = {
    .name = "encrypt",
    .usage =
        "Usage: tspdf encrypt <input.pdf> -o <output.pdf> [--password <pass>]\n"
        "                     [--password-file <path>] [--owner-password <pass>]\n"
        "                     [--owner-password-file <path>] [--bits 128|256]\n"
        "                     [--permissions <list>]\n"
        "\nEncrypt a PDF with a password.\n"
        "The password can be passed with --password, read from the first line of a\n"
        "file with --password-file ('-' reads stdin), or typed at a hidden prompt\n"
        "when neither is given. The owner password defaults to the user password.\n"
        "\n--permissions takes a comma-separated list of ALLOWED actions; anything\n"
        "not listed is denied. Without the flag everything is allowed. Actions:\n"
        "  print, modify, copy, annotate, forms, extract, assemble, print-hq\n"
        "Requesting print-hq also enables print (high-res print needs print).\n",
    .flags = FLAGS,
    .min_pos = 1, .max_pos = 1,
    .needs = TSPDF_CMD_NEEDS_OUTPUT | TSPDF_CMD_TAKES_PASSWORD | TSPDF_CMD_PASSWORD_REQUIRED,
    .run = run,
};
