// Generates a test PDF demonstrating GROW elements in paginated layout.
// Each page has a different number of fixed rows + a GROW section that
// fills the remaining space with nested GROW children.

#define _POSIX_C_SOURCE 199309L
#include "../src/pdf/tspdf_writer.h"
#include "../src/pdf/pdf_stream.h"
#include "../src/layout/layout.h"
#include "../src/util/arena.h"
#include <stdio.h>
#include <string.h>

static TspdfWriter *g_doc = NULL;

static double measure_text_cb(const char *font_name, double font_size,
                               const char *text, void *userdata) {
    (void)userdata;
    double w = tspdf_writer_measure_text(g_doc, font_name, font_size, text);
    if (w > 0) return w;
    return strlen(text) * font_size * 0.5;
}

static double font_line_height_cb(const char *font_name, double font_size,
                                   void *userdata) {
    (void)userdata;
    TTF_Font *ttf = tspdf_writer_get_ttf(g_doc, font_name);
    if (ttf) return ttf_get_line_height(ttf, font_size);
    const TspdfBase14Metrics *b14 = tspdf_writer_get_base14(g_doc, font_name);
    if (b14) return tspdf_base14_line_height(b14, font_size);
    return font_size * 1.2;
}

// Build a layout: fixed rows + a GROW section with nested GROW children
static TspdfNode *build_layout(TspdfLayout *ctx, const char *font, int fixed_row_count) {
    TspdfNode *root = tspdf_layout_box(ctx);
    root->width = tspdf_size_grow();
    root->height = tspdf_size_fit();
    root->direction = TSPDF_DIR_COLUMN;
    root->gap = 4;
    root->padding = tspdf_padding_all(20);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,root);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.96, 0.96, 0.96);
    }

    // Repeating header
    TspdfNode *hdr = tspdf_layout_box(ctx);
    hdr->width = tspdf_size_grow();
    hdr->height = tspdf_size_fixed(32);
    hdr->direction = TSPDF_DIR_ROW;
    hdr->align_y = TSPDF_ALIGN_CENTER;
    hdr->padding = tspdf_padding_xy(12, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,hdr);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.12, 0.12, 0.3);
    }
    hdr->repeat_mode = TSPDF_REPEAT_ALL;
    tspdf_layout_add_child(root, hdr);

    char hdr_buf[128];
    snprintf(hdr_buf, sizeof(hdr_buf), "Paginated GROW Demo — %d fixed rows", fixed_row_count);
    TspdfNode *ht = tspdf_layout_text(ctx, hdr_buf, font, 11);
    ht->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(hdr, ht);

    // Fixed data rows
    for (int i = 0; i < fixed_row_count; i++) {
        TspdfNode *row = tspdf_layout_box(ctx);
        row->width = tspdf_size_grow();
        row->height = tspdf_size_fixed(28);
        row->direction = TSPDF_DIR_ROW;
        row->align_y = TSPDF_ALIGN_CENTER;
        row->padding = tspdf_padding_xy(10, 0);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(ctx,row);
            s->has_background = true;
            s->background = (i % 2)
                ? tspdf_color_rgb(1, 1, 1)
                : tspdf_color_rgb(0.92, 0.92, 0.98);
        }
        tspdf_layout_add_child(root, row);

        char buf[64];
        snprintf(buf, sizeof(buf), "Data row %d", i + 1);
        TspdfNode *t = tspdf_layout_text(ctx, buf, font, 10);
        tspdf_layout_add_child(row, t);
    }

    // GROW section — fills remaining page space
    TspdfNode *sec = tspdf_layout_box(ctx);
    sec->width = tspdf_size_grow();
    sec->height = tspdf_size_grow();
    sec->direction = TSPDF_DIR_COLUMN;
    sec->gap = 0;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,sec);
        s->has_border = true;
        s->border_color = tspdf_color_rgb(0.2, 0.2, 0.55);
        s->border_width = 2;
    }
    tspdf_layout_add_child(root, sec);

    // Title bar (fixed)
    TspdfNode *title = tspdf_layout_box(ctx);
    title->width = tspdf_size_grow();
    title->height = tspdf_size_fixed(35);
    title->align_x = TSPDF_ALIGN_CENTER;
    title->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,title);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.18, 0.32, 0.7);
    }
    tspdf_layout_add_child(sec, title);
    TspdfNode *tt = tspdf_layout_text(ctx, "Title Bar (fixed 35px)", font, 12);
    tt->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(title, tt);

    // GROW middle — nested GROW inside a GROW section
    TspdfNode *mid = tspdf_layout_box(ctx);
    mid->width = tspdf_size_grow();
    mid->height = tspdf_size_grow();
    mid->direction = TSPDF_DIR_ROW;
    mid->gap = 8;
    mid->padding = tspdf_padding_all(10);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,mid);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.88, 0.92, 1.0);
    }
    tspdf_layout_add_child(sec, mid);

    // Left column: two green boxes at 2:1 weight ratio
    TspdfNode *left = tspdf_layout_box(ctx);
    left->width = tspdf_size_percent(0.5);
    left->height = tspdf_size_grow();
    left->direction = TSPDF_DIR_COLUMN;
    left->gap = 8;
    tspdf_layout_add_child(mid, left);

    TspdfNode *big = tspdf_layout_box(ctx);
    big->width = tspdf_size_grow();
    big->height = tspdf_size_grow_weight(2);
    big->align_x = TSPDF_ALIGN_CENTER;
    big->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,big);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.22, 0.58, 0.22);
        s->corner_radius[0] = 12;
        s->corner_radius[1] = 12;
        s->corner_radius[2] = 12;
        s->corner_radius[3] = 12;
    }
    tspdf_layout_add_child(left, big);
    TspdfNode *bigt = tspdf_layout_text(ctx, "GROW weight=2", font, 18);
    bigt->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(big, bigt);

    TspdfNode *sml = tspdf_layout_box(ctx);
    sml->width = tspdf_size_grow();
    sml->height = tspdf_size_grow_weight(1);
    sml->align_x = TSPDF_ALIGN_CENTER;
    sml->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,sml);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.15, 0.42, 0.15);
        s->corner_radius[0] = 12;
        s->corner_radius[1] = 12;
        s->corner_radius[2] = 12;
        s->corner_radius[3] = 12;
    }
    tspdf_layout_add_child(left, sml);
    TspdfNode *smlt = tspdf_layout_text(ctx, "GROW weight=1", font, 18);
    smlt->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(sml, smlt);

    // Right column: descriptive text
    TspdfNode *right = tspdf_layout_box(ctx);
    right->width = tspdf_size_percent(0.5);
    right->height = tspdf_size_grow();
    right->direction = TSPDF_DIR_COLUMN;
    right->gap = 12;
    right->padding = tspdf_padding_all(16);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,right);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 0.97, 0.92);
        s->corner_radius[0] = 10;
        s->corner_radius[1] = 10;
        s->corner_radius[2] = 10;
        s->corner_radius[3] = 10;
    }
    tspdf_layout_add_child(mid, right);

    TspdfNode *d1 = tspdf_layout_text(ctx, "This section has height = GROW.", font, 12);
    d1->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(right, d1);
    TspdfNode *d2 = tspdf_layout_text(ctx, "It expands to fill all remaining page space after the fixed rows above.", font, 11);
    d2->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(right, d2);
    TspdfNode *d3 = tspdf_layout_text(ctx, "The green panels have GROW weights 2:1, so the top is twice the height of the bottom.", font, 11);
    d3->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(right, d3);

    // Footer bar (fixed)
    TspdfNode *ftr = tspdf_layout_box(ctx);
    ftr->width = tspdf_size_grow();
    ftr->height = tspdf_size_fixed(35);
    ftr->align_x = TSPDF_ALIGN_CENTER;
    ftr->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(ctx,ftr);
        s->has_background = true;
        s->background = tspdf_color_rgb(0.18, 0.32, 0.7);
    }
    tspdf_layout_add_child(sec, ftr);
    TspdfNode *ft = tspdf_layout_text(ctx, "Footer Bar (fixed 35px)", font, 12);
    ft->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(ftr, ft);

    return root;
}

int main(void) {
    TspdfArena arena = tspdf_arena_create(32 * 1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    TspdfWriter *doc = tspdf_writer_create();
    g_doc = doc;
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    ctx.measure_text = measure_text_cb;
    ctx.measure_userdata = NULL;
    ctx.font_line_height = font_line_height_cb;
    ctx.line_height_userdata = NULL;
    ctx.doc = doc;

    printf("Generating grow_test.pdf ...\n\n");

    int row_counts[] = {3, 8, 15};
    int num_variants = 3;

    for (int v = 0; v < num_variants; v++) {
        TspdfNode *root = build_layout(&ctx, font, row_counts[v]);
        TspdfPaginationResult result;
        tspdf_layout_compute_paginated(&ctx, root, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT, &result);

        for (int p = 0; p < result.page_count; p++) {
            TspdfStream *s = tspdf_writer_add_page(doc);
            tspdf_layout_render_page_recompute(&ctx, root, &result, p, s);

            // Report GROW section heights
            int start = result.pages[p].start, end = result.pages[p].end;
            for (int i = start; i < end; i++) {
                int ci = result.content_indices[i];
                TspdfNode *c = root->children[ci];
                if (c->height.mode == TSPDF_SIZE_GROW) {
                    printf("  %d rows: section=%.0fpx", row_counts[v],
                           c->computed_height);
                    for (int j = 0; j < c->child_count; j++)
                        if (c->children[j]->height.mode == TSPDF_SIZE_GROW)
                            printf(", middle=%.0fpx",
                                   c->children[j]->computed_height);
                    printf("\n");
                }
            }
        }
        tspdf_layout_tree_free(root);
    }

    tspdf_writer_save(doc, "grow_test.pdf");
    printf("\nSaved grow_test.pdf (%d pages)\n", 3);

    tspdf_writer_destroy(doc);
    tspdf_arena_destroy(&arena);
    return 0;
}
