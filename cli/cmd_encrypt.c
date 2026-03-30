#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_encrypt(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf encrypt <input.pdf> -o <output.pdf> --password <pass>\n");
        printf("                     [--owner-password <pass>] [--bits 128|256]\n");
        printf("\nEncrypt a PDF with a password.\n");
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

    const char *password = find_flag(argc, argv, "--password");
    if (!password) {
        fprintf(stderr, "tspdf encrypt: missing --password <pass>\n");
        return 1;
    }

    const char *owner_pass = find_flag(argc, argv, "--owner-password");
    if (!owner_pass) owner_pass = password;

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
