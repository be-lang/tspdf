#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        printf("Usage: tspdf img2pdf <image1> <image2> [...] -o <output.pdf> [--best-effort]\n");
        printf("\nConvert JPEG/PNG images into a multi-page PDF (one image per page).\n");
        printf("Each image is scaled to fit the page, keeping its aspect ratio, and centered.\n");
        printf("Exits non-zero if any input fails to load (the pages that loaded are still\n");
        printf("written). Pass --best-effort to skip unsupported inputs and exit 0.\n");
        return argc == 0 ? 1 : 0;
    }

    bool best_effort = has_flag(argc, argv, "--best-effort");

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf img2pdf: missing -o <output.pdf>\n");
        return 1;
    }

    // argc bounds the number of positionals, so an argc-sized array can never
    // truncate the list (a fixed cap used to silently drop images past 64).
    const char **images = calloc((size_t)argc, sizeof(*images));
    if (!images) { fprintf(stderr, "tspdf img2pdf: out of memory\n"); return 1; }
    int nimg = collect_positional(argc, argv, images, argc);
    if (nimg == 0) {
        fprintf(stderr, "tspdf img2pdf: no input images specified\n");
        free(images);
        return 1;
    }

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) {
        fprintf(stderr, "tspdf img2pdf: out of memory\n");
        free(images);
        return 1;
    }

    tspdf_writer_set_title(doc, "Image Collection");

    int pages_added = 0;
    int load_failures = 0;
    for (int i = 0; i < nimg; i++) {
        const char *img_name = NULL;
        if (is_png(images[i])) {
            img_name = tspdf_writer_add_png_image(doc, images[i]);
        } else {
            img_name = tspdf_writer_add_jpeg_image(doc, images[i]);
        }

        if (!img_name) {
            // The writer's image table is a hard limit: bail out instead of
            // silently writing a PDF that is missing the remaining inputs.
            // --best-effort does not apply — that flag skips broken images,
            // it must not paper over dropped good ones.
            if (doc->last_error == TSPDF_ERR_IMAGE_LIMIT) {
                fprintf(stderr, "tspdf img2pdf: too many images (limit %d, got %d)\n",
                        TSPDF_MAX_IMAGES, nimg);
                tspdf_writer_destroy(doc);
                free(images);
                return 1;
            }
            fprintf(stderr, "tspdf img2pdf: failed to load '%s'\n", images[i]);
            load_failures++;
            continue;
        }

        TspdfStream *page = tspdf_writer_add_page(doc);
        double pw = TSPDF_PAGE_A4_WIDTH;
        double ph = TSPDF_PAGE_A4_HEIGHT;
        double margin = 36;
        double aw = pw - 2 * margin;
        double ah = ph - 2 * margin;

        // Scale to fit the content box preserving aspect ratio, centered.
        // The image just added is the last entry in the writer's image table.
        const TspdfImage *img = &doc->images[doc->image_count - 1];
        double sx = aw / img->width;
        double sy = ah / img->height;
        double scale = sx < sy ? sx : sy;
        double dw = img->width * scale;
        double dh = img->height * scale;
        double dx = margin + (aw - dw) / 2;
        double dy = margin + (ah - dh) / 2;

        tspdf_stream_draw_image(page, img_name, dx, dy, dw, dh);
        pages_added++;
    }

    free(images);

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

    // Signal partial failure: some inputs could not be loaded. The pages that
    // did load are still written above, but we must not exit 0 and let a
    // silently-dropped page pass unnoticed — unless --best-effort was given.
    if (load_failures > 0 && !best_effort) {
        fprintf(stderr, "tspdf img2pdf: %d of %d image(s) failed to load "
                        "(pass --best-effort to ignore)\n", load_failures, nimg);
        return 1;
    }

    return 0;
}
