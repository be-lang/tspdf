#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>

int cmd_info(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf info <input.pdf> [--password <pass>]\n");
        printf("\nPrint information about a PDF file.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf info: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf info: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];
    const char *password = find_flag(argc, argv, "--password");

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);

    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            printf("File:      %s\n", input);
            printf("Encrypted: yes\n");
            printf("\nThis PDF is encrypted. Use --password <pass> to open it.\n");
            return 0;
        }
        fprintf(stderr, "tspdf info: failed to open '%s': %s\n", input, tspdf_error_string(err));
        return 1;
    }

    size_t page_count = tspdf_reader_page_count(doc);

    printf("File:       %s\n", input);
    printf("Pages:      %zu\n", page_count);

    // Show page sizes
    if (page_count > 0 && page_count <= 5) {
        for (size_t i = 0; i < page_count; i++) {
            TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
            if (page) {
                double unit = page->user_unit > 0.0 ? page->user_unit : 1.0;
                double w = (page->media_box[2] - page->media_box[0]) * unit;
                double h = (page->media_box[3] - page->media_box[1]) * unit;
                printf("  Page %zu:  %.1f x %.1f pt", i + 1, w, h);
                if (unit != 1.0)
                    printf(" (/UserUnit %.4g)", unit);
                if (page->rotate != 0)
                    printf(" (rotated %d°)", page->rotate);
                printf("\n");
            }
        }
    } else if (page_count > 5) {
        TspdfReaderPage *first = tspdf_reader_get_page(doc, 0);
        TspdfReaderPage *last = tspdf_reader_get_page(doc, page_count - 1);
        if (first) {
            double unit = first->user_unit > 0.0 ? first->user_unit : 1.0;
            double w = (first->media_box[2] - first->media_box[0]) * unit;
            double h = (first->media_box[3] - first->media_box[1]) * unit;
            printf("  Page 1:  %.1f x %.1f pt", w, h);
            if (unit != 1.0)
                printf(" (/UserUnit %.4g)", unit);
            printf("\n");
        }
        printf("  ...\n");
        if (last) {
            double unit = last->user_unit > 0.0 ? last->user_unit : 1.0;
            double w = (last->media_box[2] - last->media_box[0]) * unit;
            double h = (last->media_box[3] - last->media_box[1]) * unit;
            printf("  Page %zu: %.1f x %.1f pt", page_count, w, h);
            if (unit != 1.0)
                printf(" (/UserUnit %.4g)", unit);
            printf("\n");
        }
    }

    printf("Encrypted:  %s\n", password ? "yes (opened with password)" : "no");

    tspdf_reader_destroy(doc);
    return 0;
}
