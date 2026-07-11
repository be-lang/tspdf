#include "commands.h"
#include "../include/tspdf.h"
#include "password_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_merge(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf merge <file1.pdf> <file2.pdf> [...] -o <output.pdf>\n");
        printf("\nMerge multiple PDF files into one.\n");
        printf("Encrypted files: pass --password <pass> or --password-file <file>.\n");
        printf("The one password is tried on every input (unencrypted inputs ignore\n");
        printf("it); an input it does not unlock is an error.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf merge: missing -o <output.pdf>\n");
        return 1;
    }

    // Collect input files. collect_positional already skips value-taking flag
    // arguments (such as the -o output path), so the positionals are the inputs.
    // argc bounds the number of positionals, so an argc-sized array can never
    // truncate the list (a fixed cap used to silently drop inputs past 64).
    const char **inputs = calloc((size_t)argc, sizeof(*inputs));
    if (!inputs) {
        fprintf(stderr, "tspdf merge: out of memory\n");
        return 1;
    }
    int npos = collect_positional(argc, argv, inputs, argc);

    if (npos < 2) {
        fprintf(stderr, "tspdf merge: need at least 2 input files\n");
        free(inputs);
        return 1;
    }

    TspdfReader **docs = calloc((size_t)npos, sizeof(TspdfReader *));
    if (!docs) {
        fprintf(stderr, "tspdf merge: out of memory\n");
        free(inputs);
        return 1;
    }

    // A single --password/--password-file is tried on every input: harmless
    // for unencrypted inputs (the password is ignored), and a clear error
    // names the input it fails to unlock.
    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "merge", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    int ret = 0;
    TspdfError err = TSPDF_OK;

    for (int i = 0; i < npos; i++) {
        docs[i] = password
            ? tspdf_reader_open_file_with_password(inputs[i], password, &err)
            : tspdf_reader_open_file(inputs[i], &err);
        if (!docs[i]) {
            if (err == TSPDF_ERR_ENCRYPTED) {
                fprintf(stderr, "tspdf merge: '%s' is encrypted; use --password or "
                                "--password-file (applied to every input)\n", inputs[i]);
            } else if (err == TSPDF_ERR_BAD_PASSWORD) {
                fprintf(stderr, "tspdf merge: wrong password for '%s' (the one "
                                "--password is tried on every input)\n", inputs[i]);
            } else {
                fprintf(stderr, "tspdf merge: failed to open '%s': %s\n",
                        inputs[i], tspdf_error_string(err));
            }
            ret = 1;
            goto cleanup;
        }
    }

    TspdfReader *merged = tspdf_reader_merge(docs, (size_t)npos, &err);
    if (!merged) {
        fprintf(stderr, "tspdf merge: merge failed: %s\n", tspdf_error_string(err));
        ret = 1;
        goto cleanup;
    }

    err = tspdf_reader_save(merged, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf merge: failed to save '%s': %s\n", output, tspdf_error_string(err));
        ret = 1;
    } else {
        printf("Merged %d files → %s\n", npos, output);
    }

    tspdf_reader_destroy(merged);

cleanup:
    for (int i = 0; i < npos; i++) {
        if (docs[i]) tspdf_reader_destroy(docs[i]);
    }
    free(docs);
    free(inputs);
    return ret;
}
