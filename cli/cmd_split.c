#include "commands.h"
#include "../include/tspdf.h"
#include "password_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Open `input`, honoring an optional password; prints the error (with the
// standard --password hint for encrypted files) on failure.
static TspdfReader *split_open(const char *input, const char *password) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf split: '%s' is encrypted; use --password or --password-file\n", input);
        } else {
            fprintf(stderr, "tspdf split: failed to open '%s': %s\n", input, tspdf_error_string(err));
        }
    }
    return doc;
}

// "<n>" zero-padded to `width` digits (clamped to what a size_t needs).
static void zero_pad(char *dst, size_t dst_sz, size_t n, int width) {
    char digits[24];
    int nd = 0;
    do { digits[nd++] = (char)('0' + n % 10); n /= 10; } while (n > 0);
    size_t out = 0;
    for (int pad = width - nd; pad > 0 && out + 1 < dst_sz; pad--) dst[out++] = '0';
    while (nd > 0 && out + 1 < dst_sz) dst[out++] = digits[--nd];
    dst[out] = '\0';
}

// Drop every document-level embedded file from `doc` (for --no-attachments;
// extract copies attachments into each part by default). Returns TSPDF_OK
// when there was nothing to drop.
static TspdfError split_drop_attachments(TspdfReader *doc) {
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_attachments(doc, &infos, &count);
    for (size_t i = 0; i < count && err == TSPDF_OK; i++) {
        err = tspdf_reader_attachment_remove(doc, infos[i].name);
    }
    return err;
}

// Burst mode: write every page of `doc` to its own file. `output` is the
// name template: "out.pdf" becomes out-001.pdf, out-002.pdf, ... zero-padded
// to the page-count width (minimum 3 digits).
static int split_burst(TspdfReader *doc, const char *output, bool no_attachments) {
    size_t total = tspdf_reader_page_count(doc);
    if (total == 0) {
        fprintf(stderr, "tspdf split: document has no pages\n");
        return 1;
    }

    // Base name = output minus a trailing ".pdf" (case-insensitive).
    size_t base_len = strlen(output);
    if (base_len >= 4 && strcasecmp(output + base_len - 4, ".pdf") == 0)
        base_len -= 4;

    int width = 1;
    for (size_t t = total; t >= 10; t /= 10) width++;
    if (width < 3) width = 3;

    char *name = malloc(base_len + 32);
    char *first_name = malloc(base_len + 32);
    if (!name || !first_name) {
        fprintf(stderr, "tspdf split: out of memory\n");
        free(name);
        free(first_name);
        return 1;
    }

    for (size_t i = 0; i < total; i++) {
        char num[24];
        zero_pad(num, sizeof(num), i + 1, width);
        memcpy(name, output, base_len);
        snprintf(name + base_len, 32, "-%s.pdf", num);
        if (i == 0) memcpy(first_name, name, strlen(name) + 1);

        TspdfError err = TSPDF_OK;
        TspdfReader *page = tspdf_reader_extract(doc, &i, 1, &err);
        if (!page) {
            fprintf(stderr, "tspdf split: extract of page %zu failed: %s\n",
                    i + 1, tspdf_error_string(err));
            free(name);
            free(first_name);
            return 1;
        }
        if (no_attachments && (err = split_drop_attachments(page)) != TSPDF_OK) {
            fprintf(stderr, "tspdf split: dropping attachments failed: %s\n",
                    tspdf_error_string(err));
            tspdf_reader_destroy(page);
            free(name);
            free(first_name);
            return 1;
        }

        // Only write objects reachable from this page (same rationale as the
        // strip_unused_objects note in the --pages path below).
        TspdfSaveOptions opts = tspdf_save_options_default();
        opts.strip_unused_objects = true;
        err = tspdf_reader_save_with_options(page, name, &opts);
        tspdf_reader_destroy(page);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf split: failed to save '%s': %s\n",
                    name, tspdf_error_string(err));
            free(name);
            free(first_name);
            return 1;
        }
    }

    printf("Split %zu page(s) → %s .. %s\n", total, first_name, name);
    free(name);
    free(first_name);
    return 0;
}

// First 0-based page index in `pages` that is out of range for `total`.
int cmd_split(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf split <input.pdf> [--pages 1-3,5 | --burst] [--no-attachments] -o <output.pdf>\n");
        printf("\nExtract specific pages from a PDF, or split it into one file per page.\n");
        printf("With --pages, the selected pages are written to the output file.\n");
        printf("With --burst (the default when --pages is absent), each page is written\n");
        printf("to its own file: out.pdf becomes out-001.pdf, out-002.pdf, ...\n");
        printf("Embedded file attachments are copied into every output; --no-attachments\n");
        printf("drops them instead.\n");
        printf("Encrypted files: pass --password <pass> or --password-file <file>;\n");
        printf("every output keeps the original encryption.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf split: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf split: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf split: missing -o <output.pdf>\n");
        return 1;
    }

    const char *pages_spec = find_flag(argc, argv, "--pages");
    bool no_attachments = has_flag(argc, argv, "--no-attachments");
    if (has_flag(argc, argv, "--burst") && pages_spec) {
        fprintf(stderr, "tspdf split: --burst and --pages are mutually exclusive\n");
        return 1;
    }
    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "split", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    if (!pages_spec) {
        // Per-page split (burst) is the default when no page range is given.
        TspdfReader *doc = split_open(input, password);
        if (!doc) return 1;
        int rc = split_burst(doc, output, no_attachments);
        tspdf_reader_destroy(doc);
        return rc;
    }

    size_t page_count;
    size_t *pages = parse_page_range(pages_spec, &page_count);
    if (!pages) {
        fprintf(stderr, "tspdf split: invalid page range '%s'\n", pages_spec);
        return 1;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = split_open(input, password);
    if (!doc) {
        free(pages);
        return 1;
    }

    TspdfReader *result = tspdf_reader_extract(doc, pages, page_count, &err);
    if (!result) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            size_t total = tspdf_reader_page_count(doc);
            fprintf(stderr, "tspdf split: page %zu is out of range (document has %zu page%s)\n",
                    first_out_of_range(pages, page_count, total) + 1, total,
                    total == 1 ? "" : "s");
        } else {
            fprintf(stderr, "tspdf split: extract failed: %s\n", tspdf_error_string(err));
        }
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    if (no_attachments) {
        err = split_drop_attachments(result);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf split: dropping attachments failed: %s\n",
                    tspdf_error_string(err));
            tspdf_reader_destroy(result);
            tspdf_reader_destroy(doc);
            free(pages);
            return 1;
        }
    }

    // Only write objects reachable from the extracted pages; without this the
    // output would carry every object of the source document along.
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    err = tspdf_reader_save_with_options(result, output, &opts);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf split: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    printf("Extracted %zu page(s) → %s\n", page_count, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pages);
    return 0;
}
