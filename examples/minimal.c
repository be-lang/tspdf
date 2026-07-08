// Minimal tspdf example — generates a one-page PDF with text
// Build: gcc -o minimal examples/minimal.c src/**/*.c -lm

#include "../include/tspdf.h"
#include <stdio.h>

static double measure_cb(const char *font, double size, const char *text, void *ud) {
    return tspdf_writer_measure_text((TspdfWriter *)ud, font, size, text);
}

int main(void) {
    TspdfWriter *doc = tspdf_writer_create();

    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");

    TspdfArena arena = tspdf_arena_create(1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = measure_cb;
    ctx.measure_userdata = doc;
    ctx.doc = doc;

    // Page root
    TspdfNode *root = tspdf_layout_box(&ctx);
    root->width = tspdf_size_fixed(595);    // A4 width in points
    root->height = tspdf_size_fixed(842);   // A4 height
    root->direction = TSPDF_DIR_COLUMN;
    root->padding = tspdf_padding_all(50);
    root->gap = 15;

    // Title
    TspdfNode *title = tspdf_layout_text(&ctx, "Hello from tspdf!", bold, 28);
    title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root, title);

    // Body text
    TspdfNode *body = tspdf_layout_text(&ctx,
        "This PDF was generated from pure C with zero external dependencies. "
        "No zlib, no libpng, no freetype \xe2\x80\x94 just libc.",
        font, 13);
    body->text.color = tspdf_color_from_u8(60, 60, 80);
    tspdf_layout_add_child(root, body);

    // Unicode demo
    TspdfNode *unicode = tspdf_layout_text(&ctx,
        "Unicode: caf\xC3\xA9, na\xC3\xAFve, \xC3\xBC" "ber, \xC2\xA9 2026, \xE2\x82\xAC" "42",
        font, 13);
    unicode->text.color = tspdf_color_from_u8(60, 60, 80);
    tspdf_layout_add_child(root, unicode);

    // Render
    TspdfStream *page = tspdf_writer_add_page(doc);
    tspdf_layout_compute(&ctx, root, 595, 842);
    tspdf_layout_render(&ctx, root, page);

    if (tspdf_writer_save(doc, "minimal.pdf") == TSPDF_OK) {
        printf("Written: minimal.pdf\n");
    }

    tspdf_writer_destroy(doc);
    tspdf_layout_tree_free(root);
    tspdf_arena_destroy(&arena);
}
