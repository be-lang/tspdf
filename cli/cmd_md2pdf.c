#include "commands.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TspdfWriter *g_doc = NULL;

static double measure_cb(const char *f, double s, const char *t, void *u) {
    (void)u;
    double w = tspdf_writer_measure_text(g_doc, f, s, t);
    return w > 0 ? w : (double)strlen(t) * s * 0.5;
}

static double lh_cb(const char *f, double s, void *u) {
    (void)u;
    TTF_Font *ttf = tspdf_writer_get_ttf(g_doc, f);
    if (ttf) return ttf_get_line_height(ttf, s);
    const TspdfBase14Metrics *b14 = tspdf_writer_get_base14(g_doc, f);
    if (b14) return tspdf_base14_line_height(b14, s);
    return s * 1.2;
}

int cmd_md2pdf(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf md2pdf <input.md> -o <output.pdf>\n");
        printf("\nConvert a Markdown document to a styled PDF.\n");
        return argc == 0 ? 1 : 0;
    }

    const char *positional[1];
    int npos = collect_positional(argc, argv, positional, 1);
    if (npos < 1) {
        fprintf(stderr, "tspdf md2pdf: missing input file\n");
        return 1;
    }
    const char *input = positional[0];

    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf md2pdf: missing -o <output.pdf>\n");
        return 1;
    }

    /* Read input file */
    FILE *f = fopen(input, "r");
    if (!f) {
        fprintf(stderr, "tspdf md2pdf: cannot open '%s'\n", input);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {
        fprintf(stderr, "tspdf md2pdf: file too large or empty\n");
        fclose(f);
        return 1;
    }
    char *md = malloc((size_t)fsize + 1);
    if (!md) { fclose(f); fprintf(stderr, "tspdf md2pdf: out of memory\n"); return 1; }
    fread(md, 1, (size_t)fsize, f);
    md[fsize] = '\0';
    fclose(f);

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { free(md); fprintf(stderr, "tspdf md2pdf: out of memory\n"); return 1; }
    g_doc = doc;

    tspdf_writer_set_title(doc, "Document");
    tspdf_writer_set_creator(doc, "tspdf");

    const char *sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");
    const char *mono = tspdf_writer_add_builtin_font(doc, "Courier");

    TspdfArena arena = tspdf_arena_create(8 * 1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = measure_cb;
    ctx.font_line_height = lh_cb;
    ctx.doc = doc;

    double W = TSPDF_PAGE_A4_WIDTH, H = TSPDF_PAGE_A4_HEIGHT;

    TspdfNode *root = tspdf_layout_box(&ctx);
    root->width = tspdf_size_fixed(W);
    root->height = tspdf_size_fit();
    root->direction = TSPDF_DIR_COLUMN;
    root->padding = tspdf_padding_all(52);
    root->gap = 8;
    TspdfBoxStyle *root_style = tspdf_layout_node_style(&ctx, root);
    root_style->has_background = true;
    root_style->background = tspdf_color_rgb(1, 1, 1);

    /* Parse markdown line by line */
    int in_code_block = 0;
    char code_buf[8192];
    int code_len = 0;

    char *line = md;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        /* Temporary null-terminate the line */
        char saved = line[line_len];
        line[line_len] = '\0';

        /* Strip trailing \r */
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        if (!in_code_block && strncmp(line, "```", 3) == 0) {
            in_code_block = 1;
            code_len = 0;
            code_buf[0] = '\0';
        } else if (in_code_block && strncmp(line, "```", 3) == 0) {
            in_code_block = 0;
            /* Emit code block */
            TspdfNode *code_box = tspdf_layout_box(&ctx);
            code_box->width = tspdf_size_grow();
            code_box->direction = TSPDF_DIR_COLUMN;
            code_box->padding = tspdf_padding_all(8);
            TspdfBoxStyle *cs = tspdf_layout_node_style(&ctx, code_box);
            cs->has_background = true;
            cs->background = tspdf_color_from_u8(240, 242, 250);

            TspdfNode *code_txt = tspdf_layout_text(&ctx, code_buf, mono, 10);
            code_txt->text.color = tspdf_color_from_u8(60, 40, 120);
            code_txt->width = tspdf_size_grow();
            tspdf_layout_add_child(code_box, code_txt);
            tspdf_layout_add_child(root, code_box);
        } else if (in_code_block) {
            /* Accumulate code lines */
            if (code_len > 0 && code_len < (int)sizeof(code_buf) - 2) {
                code_buf[code_len++] = '\n';
            }
            int remaining = (int)sizeof(code_buf) - code_len - 1;
            if (remaining > 0) {
                int copy = (int)line_len < remaining ? (int)line_len : remaining;
                memcpy(code_buf + code_len, line, copy);
                code_len += copy;
                code_buf[code_len] = '\0';
            }
        } else if (line[0] == '#') {
            /* Heading */
            int level = 0;
            while (line[level] == '#') level++;
            const char *text = line + level;
            while (*text == ' ') text++;

            double sizes[] = {24, 18, 14};
            double sz = level <= 3 ? sizes[level - 1] : 12;

            TspdfNode *node = tspdf_layout_text(&ctx, text, bold, sz);
            node->text.color = tspdf_color_from_u8(10, 15, 50);
            node->width = tspdf_size_grow();
            tspdf_layout_add_child(root, node);
        } else if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            /* List item */
            TspdfNode *row = tspdf_layout_box(&ctx);
            row->direction = TSPDF_DIR_ROW;
            row->width = tspdf_size_grow();
            row->gap = 8;

            TspdfNode *bullet = tspdf_layout_text(&ctx, "\xe2\x80\xa2", sans, 11);
            bullet->text.color = tspdf_color_from_u8(79, 110, 247);
            tspdf_layout_add_child(row, bullet);

            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = tspdf_color_from_u8(40, 50, 80);
            tspdf_layout_add_child(row, txt);

            tspdf_layout_add_child(root, row);
        } else if (line[0] == '>' && line[1] == ' ') {
            /* Blockquote */
            TspdfNode *bq = tspdf_layout_box(&ctx);
            bq->width = tspdf_size_grow();
            bq->direction = TSPDF_DIR_COLUMN;
            bq->padding = (TspdfPadding){12, 8, 8, 8};
            TspdfBoxStyle *bs = tspdf_layout_node_style(&ctx, bq);
            bs->has_background = true;
            bs->background = tspdf_color_from_u8(245, 245, 250);
            bs->has_border_left = true;
            bs->border_left = 3;
            bs->border_color_left = tspdf_color_from_u8(79, 110, 247);

            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = tspdf_color_from_u8(80, 90, 110);
            tspdf_layout_add_child(bq, txt);

            tspdf_layout_add_child(root, bq);
        } else if ((strncmp(line, "---", 3) == 0 || strncmp(line, "***", 3) == 0) && line_len <= 5) {
            /* Horizontal rule — render as a thin box */
            TspdfNode *hr = tspdf_layout_box(&ctx);
            hr->width = tspdf_size_grow();
            hr->height = tspdf_size_fixed(1);
            TspdfBoxStyle *hs = tspdf_layout_node_style(&ctx, hr);
            hs->has_background = true;
            hs->background = tspdf_color_from_u8(200, 205, 220);
            tspdf_layout_add_child(root, hr);
        } else if (line_len > 0) {
            /* Paragraph */
            TspdfNode *node = tspdf_layout_text(&ctx, line, sans, 11);
            node->text.wrap = TSPDF_WRAP_WORD;
            node->text.color = tspdf_color_from_u8(40, 50, 80);
            node->width = tspdf_size_grow();
            tspdf_layout_add_child(root, node);
        }
        /* Skip blank lines (no output) */

        line[line_len] = saved;
        line = eol ? eol + 1 : NULL;
    }

    free(md);

    TspdfPaginationResult pagination;
    int num_pages = tspdf_layout_compute_paginated(&ctx, root, W, H, &pagination);
    for (int pg = 0; pg < num_pages; pg++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        tspdf_layout_render_page_recompute(&ctx, root, &pagination, pg, page);
    }

    TspdfError err = tspdf_writer_save(doc, output);
    tspdf_writer_destroy(doc);
    tspdf_arena_destroy(&arena);

    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf md2pdf: failed to save '%s': %s\n", output, tspdf_error_string(err));
        return 1;
    }

    printf("Markdown (%d page%s) -> %s\n", num_pages, num_pages == 1 ? "" : "s", output);
    return 0;
}
