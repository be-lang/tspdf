// tspdf scale: resize pages, scaling their content (not clipping). Either a
// uniform --factor (grow/shrink page + content) or --to a named page size
// (fit content into that size, aspect preserved, centered).

#include "commands.h"
#include "../include/tspdf.h"
#include "password_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Named page sizes in PDF points (1/72").
static bool named_size(const char *name, double *w, double *h) {
    if (!name) return false;
    if (strcasecmp(name, "a4") == 0)     { *w = 595.28; *h = 841.89; return true; }
    if (strcasecmp(name, "letter") == 0) { *w = 612.0;  *h = 792.0;  return true; }
    if (strcasecmp(name, "legal") == 0)  { *w = 612.0;  *h = 1008.0; return true; }
    if (strcasecmp(name, "a3") == 0)     { *w = 841.89; *h = 1190.55; return true; }
    if (strcasecmp(name, "a5") == 0)     { *w = 419.53; *h = 595.28; return true; }
    return false;
}

static void print_scale_help(void) {
    printf("Usage: tspdf scale <input.pdf> -o <output.pdf>\n");
    printf("             (--factor f | --to a4|letter|legal|a3|a5)\n");
    printf("             [--pages <range>]\n");
    printf("\nResize pages, scaling the content with them (not clipping). One of:\n");
    printf("  --factor f   uniform scale of page dimensions and content (f>0)\n");
    printf("  --to SIZE    fit content into the named page size, aspect preserved\n");
    printf("               and centered; the MediaBox becomes that size\n");
    printf("If --pages is omitted, all pages are scaled.\n");
    printf("Encrypted files: pass --password <pass> or --password-file <file>;\n");
    printf("the output keeps the original encryption.\n");
}

int cmd_scale(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        print_scale_help();
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf scale: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf scale: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf scale: missing -o <output.pdf>\n");
        return 1;
    }

    const char *factor_str = find_flag(argc, argv, "--factor");
    const char *to_str = find_flag(argc, argv, "--to");
    if ((factor_str != NULL) + (to_str != NULL) != 1) {
        fprintf(stderr, "tspdf scale: need exactly one of --factor or --to\n");
        return 1;
    }

    double factor = 0.0, tw = 0.0, th = 0.0;
    if (factor_str) {
        char *end = NULL;
        factor = strtod(factor_str, &end);
        if (end == factor_str || *end != '\0' || !(factor > 0.0)) {
            fprintf(stderr, "tspdf scale: invalid --factor '%s' (need > 0)\n", factor_str);
            return 1;
        }
    } else {
        if (!named_size(to_str, &tw, &th)) {
            fprintf(stderr, "tspdf scale: unknown --to size '%s' (a4, letter, legal, a3, a5)\n", to_str);
            return 1;
        }
    }

    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "scale", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf scale: '%s' is encrypted; use --password or --password-file\n", input);
        } else {
            fprintf(stderr, "tspdf scale: failed to open '%s': %s\n", input, tspdf_error_string(err));
        }
        return 1;
    }

    size_t total = tspdf_reader_page_count(doc);
    size_t page_count = 0;
    size_t *pages = NULL;
    const char *pages_spec = find_flag(argc, argv, "--pages");
    if (pages_spec) {
        pages = parse_page_range(pages_spec, &page_count);
        if (!pages) {
            fprintf(stderr, "tspdf scale: invalid page range '%s'\n", pages_spec);
            tspdf_reader_destroy(doc);
            return 1;
        }
        size_t bad = first_out_of_range(pages, page_count, total);
        if (bad != total) {
            fprintf(stderr, "tspdf scale: page %zu is out of range (document has %zu page%s)\n",
                    bad + 1, total, total == 1 ? "" : "s");
            free(pages);
            tspdf_reader_destroy(doc);
            return 1;
        }
    } else {
        pages = malloc((total ? total : 1) * sizeof(size_t));
        if (!pages) {
            fprintf(stderr, "tspdf scale: out of memory\n");
            tspdf_reader_destroy(doc);
            return 1;
        }
        for (size_t i = 0; i < total; i++) pages[i] = i;
        page_count = total;
    }

    TspdfReader *result = factor_str
        ? tspdf_reader_scale(doc, pages, page_count, factor, factor, &err)
        : tspdf_reader_resize_to(doc, pages, page_count, tw, th, &err);
    if (!result) {
        fprintf(stderr, "tspdf scale: scale failed: %s\n", tspdf_error_string(err));
        free(pages);
        tspdf_reader_destroy(doc);
        return 1;
    }

    err = tspdf_reader_save(result, output);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf scale: failed to save '%s': %s\n", output, tspdf_error_string(err));
        tspdf_reader_destroy(result);
        tspdf_reader_destroy(doc);
        free(pages);
        return 1;
    }

    if (factor_str)
        printf("Scaled %zu page(s) by %.4g → %s\n", page_count, factor, output);
    else
        printf("Resized %zu page(s) to %s → %s\n", page_count, to_str, output);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pages);
    return 0;
}
