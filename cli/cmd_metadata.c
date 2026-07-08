#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_field(const char *label, const char *value) {
    if (value) printf("%-16s%s\n", label, value);
}

int cmd_metadata(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf metadata <input.pdf>                                     # view\n");
        printf("       tspdf metadata <input.pdf> --set title=\"X\" --set author=\"Y\" -o out.pdf  # edit\n");
        printf("\nView or edit PDF metadata.\n");
        printf("Supported keys: title, author, subject, keywords, creator, producer\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf metadata: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    // Collect --set values
    const char *sets[32];
    int nsets = find_flags(argc, argv, "--set", sets, 32);

    const char *output = find_flag(argc, argv, "-o");

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_file(input, &err);
    if (!doc) {
        fprintf(stderr, "tspdf metadata: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    if (nsets == 0) {
        // View mode
        printf("Metadata for %s:\n", input);
        print_field("Title:", tspdf_reader_get_title(doc));
        print_field("Author:", tspdf_reader_get_author(doc));
        print_field("Subject:", tspdf_reader_get_subject(doc));
        print_field("Keywords:", tspdf_reader_get_keywords(doc));
        print_field("Creator:", tspdf_reader_get_creator(doc));
        print_field("Producer:", tspdf_reader_get_producer(doc));
        print_field("Created:", tspdf_reader_get_creation_date(doc));
        print_field("Modified:", tspdf_reader_get_mod_date(doc));
        tspdf_reader_destroy(doc);
        return 0;
    }

    // Edit mode
    if (!output) {
        fprintf(stderr, "tspdf metadata: --set requires -o <output.pdf>\n");
        tspdf_reader_destroy(doc);
        return 1;
    }

    for (int i = 0; i < nsets; i++) {
        const char *eq = strchr(sets[i], '=');
        if (!eq) {
            fprintf(stderr, "tspdf metadata: invalid --set format '%s' (expected key=value)\n", sets[i]);
            tspdf_reader_destroy(doc);
            return 1;
        }
        size_t key_len = (size_t)(eq - sets[i]);
        const char *value = eq + 1;

        if (strncmp(sets[i], "title", key_len) == 0 && key_len == 5)
            tspdf_reader_set_title(doc, value);
        else if (strncmp(sets[i], "author", key_len) == 0 && key_len == 6)
            tspdf_reader_set_author(doc, value);
        else if (strncmp(sets[i], "subject", key_len) == 0 && key_len == 7)
            tspdf_reader_set_subject(doc, value);
        else if (strncmp(sets[i], "keywords", key_len) == 0 && key_len == 8)
            tspdf_reader_set_keywords(doc, value);
        else if (strncmp(sets[i], "creator", key_len) == 0 && key_len == 7)
            tspdf_reader_set_creator(doc, value);
        else if (strncmp(sets[i], "producer", key_len) == 0 && key_len == 8)
            tspdf_reader_set_producer(doc, value);
        else {
            fprintf(stderr, "tspdf metadata: unknown key '%.*s'\n", (int)key_len, sets[i]);
            tspdf_reader_destroy(doc);
            return 1;
        }
    }

    err = tspdf_reader_save(doc, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf metadata: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("Updated %d field(s) → %s\n", nsets, output);

    tspdf_reader_destroy(doc);
    return 0;
}
