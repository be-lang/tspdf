#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>

int cmd_decrypt(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf decrypt <input.pdf> -o <output.pdf> --password <pass>\n");
        printf("\nDecrypt a password-protected PDF.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf decrypt: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf decrypt: missing -o <output.pdf>\n");
        return 1;
    }

    const char *password = find_flag(argc, argv, "--password");
    if (!password) {
        fprintf(stderr, "tspdf decrypt: missing --password <pass>\n");
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file_with_password(input, password, &err);
    if (!doc) {
        fprintf(stderr, "tspdf decrypt: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf decrypt: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("Decrypted → %s\n", output);

    tspdf_reader_destroy(doc);
    return 0;
}
