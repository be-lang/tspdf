#include "commands.h"
#include "password_input.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_encrypt(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf encrypt <input.pdf> -o <output.pdf> [--password <pass>]\n");
        printf("                     [--password-file <path>] [--owner-password <pass>]\n");
        printf("                     [--owner-password-file <path>] [--bits 128|256]\n");
        printf("\nEncrypt a PDF with a password.\n");
        printf("The password can be passed with --password, read from the first line of a\n");
        printf("file with --password-file ('-' reads stdin), or typed at a hidden prompt\n");
        printf("when neither is given. The owner password defaults to the user password.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf encrypt: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf encrypt: missing -o <output.pdf>\n");
        return 1;
    }

    char pass_buf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "encrypt", "Password: ",
                                                  true, pass_buf, sizeof(pass_buf));
    if (!password) return 1;

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

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf encrypt: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    // Allow all permissions (0xFFFFFFFF)
    err = tspdf_reader_save_encrypted(doc, output, password, owner_pass, 0xFFFFFFFF, bits);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf encrypt: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("Encrypted (AES-%d) → %s\n", bits, output);

    tspdf_reader_destroy(doc);
    return 0;
}
