#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>

#define MAX_IMAGES 64

static int is_png(const char *path) {
    size_t len = strlen(path);
    if (len < 4) return 0;
    const char *ext = path + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'n' || ext[2] == 'N') &&
            (ext[3] == 'g' || ext[3] == 'G'));
}

int cmd_img2pdf(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf img2pdf <image1> <image2> [...] -o <output.pdf>\n");
        printf("\nConvert JPEG/PNG images into a multi-page PDF (one image per page).\n");
        return argc == 0 ? 1 : 0;
    }

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf img2pdf: missing -o <output.pdf>\n");
        return 1;
    }

    const char *images[MAX_IMAGES];
    int nimg = collect_positional(argc, argv, images, MAX_IMAGES);
    if (nimg == 0) {
        fprintf(stderr, "tspdf img2pdf: no input images specified\n");
        return 1;
    }

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { fprintf(stderr, "tspdf img2pdf: out of memory\n"); return 1; }

    tspdf_writer_set_title(doc, "Image Collection");

    int pages_added = 0;
    for (int i = 0; i < nimg; i++) {
        const char *img_name = NULL;
        if (is_png(images[i])) {
            img_name = tspdf_writer_add_png_image(doc, images[i]);
        } else {
            img_name = tspdf_writer_add_jpeg_image(doc, images[i]);
        }

        if (!img_name) {
            fprintf(stderr, "tspdf img2pdf: failed to load '%s'\n", images[i]);
            continue;
        }

        TspdfStream *page = tspdf_writer_add_page(doc);
        double pw = TSPDF_PAGE_A4_WIDTH;
        double ph = TSPDF_PAGE_A4_HEIGHT;
        double margin = 36;
        double aw = pw - 2 * margin;
        double ah = ph - 2 * margin;

        tspdf_stream_draw_image(page, img_name, margin, margin, aw, ah);
        pages_added++;
    }

    if (pages_added == 0) {
        fprintf(stderr, "tspdf img2pdf: no valid images found\n");
        tspdf_writer_destroy(doc);
        return 1;
    }

    TspdfError err = tspdf_writer_save(doc, output);
    tspdf_writer_destroy(doc);

    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf img2pdf: failed to save '%s': %s\n", output, tspdf_error_string(err));
        return 1;
    }

    printf("Converted %d image(s) -> %s\n", pages_added, output);
    return 0;
}
