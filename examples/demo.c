#include "../include/tspdf.h"
#include <stdio.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Measurement callbacks that bridge layout engine ↔ PDF document ---

static double measure_text_cb(const char *font_name, double font_size, const char *text, void *userdata) {
    double w = tspdf_writer_measure_text((TspdfWriter *)userdata, font_name, font_size, text);
    if (w > 0) return w;
    // Last resort fallback (should not happen with base14 metrics)
    return strlen(text) * font_size * 0.5;
}

static double font_line_height_cb(const char *font_name, double font_size, void *userdata) {
    TspdfWriter *doc = (TspdfWriter *)userdata;
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, font_name);
    if (ttf) return ttf_get_line_height(ttf, font_size);
    const TspdfBase14Metrics *b14 = tspdf_writer_get_base14(doc, font_name);
    if (b14) return tspdf_base14_line_height(b14, font_size);
    return font_size * 1.2;
}

int main(void) {
    TspdfWriter *doc = tspdf_writer_create();

    // Load TrueType fonts
    const char *sans = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    const char *sans_bold = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationSans-Bold.ttf");
    const char *mono = tspdf_writer_add_ttf_font(doc, "/usr/share/fonts/liberation/LiberationMono-Regular.ttf");

    if (!sans || !sans_bold) {
        // Fallback to built-in fonts
        sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
        sans_bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");
        mono = tspdf_writer_add_builtin_font(doc, "Courier");
        printf("Note: Using built-in fonts (TTF fonts not found)\n");
    }

    // Setup layout system
    TspdfArena layout_arena = tspdf_arena_create(32 * 1024 * 1024);  // 32MB
    TspdfLayout ctx = tspdf_layout_create(&layout_arena);
    ctx.measure_text = measure_text_cb;
    ctx.measure_userdata = doc;
    ctx.font_line_height = font_line_height_cb;
    ctx.line_height_userdata = NULL;
    ctx.doc = doc;

    // ==========================================
    // PAGE 1: Layout engine demo
    // ==========================================
    TspdfStream *page1 = tspdf_writer_add_page(doc);

    TspdfNode *root = tspdf_layout_box(&ctx);
    root->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root->direction = TSPDF_DIR_COLUMN;
    root->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root);
        s->has_background = true;
        s->background = tspdf_color_from_u8(245, 245, 250);
    }

    // --- Header bar ---
    TspdfNode *header = tspdf_layout_box(&ctx);
    header->width = tspdf_size_grow();
    header->height = tspdf_size_fixed(70);
    header->direction = TSPDF_DIR_ROW;
    header->align_y = TSPDF_ALIGN_CENTER;
    header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root, header);

    TspdfNode *title = tspdf_layout_text(&ctx, "tspdf", sans_bold, 28);
    title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(header, title);

    TspdfNode *subtitle_spacer = tspdf_layout_box(&ctx);
    subtitle_spacer->width = tspdf_size_grow();
    tspdf_layout_add_child(header, subtitle_spacer);

    TspdfNode *version = tspdf_layout_text(&ctx, "v0.1 - Pure C, Zero Dependencies", sans, 11);
    version->text.color = tspdf_color_from_u8(166, 173, 200);
    tspdf_layout_add_child(header, version);

    // --- Content area ---
    TspdfNode *content = tspdf_layout_box(&ctx);
    content->width = tspdf_size_grow();
    content->height = tspdf_size_grow();
    content->direction = TSPDF_DIR_COLUMN;
    content->padding = tspdf_padding_all(30);
    content->gap = 20;
    tspdf_layout_add_child(root, content);

    // Section: Color boxes
    TspdfNode *section_title1 = tspdf_layout_text(&ctx, "Flexbox-Style Layout", sans_bold, 18);
    section_title1->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(content, section_title1);

    TspdfNode *box_row = tspdf_layout_box(&ctx);
    box_row->width = tspdf_size_grow();
    box_row->height = tspdf_size_fixed(80);
    box_row->direction = TSPDF_DIR_ROW;
    box_row->gap = 15;
    tspdf_layout_add_child(content, box_row);

    TspdfColor box_colors[] = {
        tspdf_color_from_u8(231, 76, 60),
        tspdf_color_from_u8(46, 204, 113),
        tspdf_color_from_u8(52, 152, 219),
        tspdf_color_from_u8(155, 89, 182),
        tspdf_color_from_u8(241, 196, 15),
    };

    for (int i = 0; i < 5; i++) {
        TspdfNode *box = tspdf_layout_box(&ctx);
        box->width = tspdf_size_grow();
        box->height = tspdf_size_grow();
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, box);
            s->has_background = true;
            s->background = box_colors[i];
            s->corner_radius[0] = 8;
            s->corner_radius[1] = 8;
            s->corner_radius[2] = 8;
            s->corner_radius[3] = 8;
        }
        box->align_x = TSPDF_ALIGN_CENTER;
        box->align_y = TSPDF_ALIGN_CENTER;
        tspdf_layout_add_child(box_row, box);

        char label[16];
        snprintf(label, sizeof(label), "%d", i + 1);
        TspdfNode *num = tspdf_layout_text(&ctx, label, sans_bold, 22);
        num->text.color = tspdf_color_rgb(1, 1, 1);
        tspdf_layout_add_child(box, num);
    }

    // Section: Text wrapping
    TspdfNode *section_title2 = tspdf_layout_text(&ctx, "Text Wrapping & TspdfAlignment", sans_bold, 18);
    section_title2->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(content, section_title2);

    TspdfNode *text_row = tspdf_layout_box(&ctx);
    text_row->width = tspdf_size_grow();
    text_row->direction = TSPDF_DIR_ROW;
    text_row->gap = 15;
    tspdf_layout_add_child(content, text_row);

    const char *sample_text = "The layout engine supports automatic word wrapping, "
        "text alignment, padding, gaps, and flexible sizing modes including "
        "fixed, grow, fit, and percentage-based layouts.";

    TspdfTextAlignment aligns[] = {TSPDF_TEXT_ALIGN_LEFT, TSPDF_TEXT_ALIGN_CENTER, TSPDF_TEXT_ALIGN_RIGHT};
    const char *align_labels[] = {"Left Aligned", "Center Aligned", "Right Aligned"};

    for (int i = 0; i < 3; i++) {
        TspdfNode *col = tspdf_layout_box(&ctx);
        col->width = tspdf_size_grow();
        col->direction = TSPDF_DIR_COLUMN;
        col->gap = 8;
        col->padding = tspdf_padding_all(15);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, col);
            s->has_background = true;
            s->background = tspdf_color_rgb(1, 1, 1);
            s->has_border = true;
            s->border_color = tspdf_color_from_u8(200, 200, 210);
            s->border_width = 1;
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        tspdf_layout_add_child(text_row, col);

        TspdfNode *label = tspdf_layout_text(&ctx, align_labels[i], sans_bold, 11);
        label->text.color = tspdf_color_from_u8(100, 100, 120);
        label->width = tspdf_size_grow();
        label->text.alignment = aligns[i];
        tspdf_layout_add_child(col, label);

        TspdfNode *body = tspdf_layout_text(&ctx, sample_text, sans, 10);
        body->text.color = tspdf_color_from_u8(60, 60, 80);
        body->width = tspdf_size_grow();
        body->text.alignment = aligns[i];
        body->text.wrap = TSPDF_WRAP_WORD;
        tspdf_layout_add_child(col, body);
    }

    // Section: Nested layout
    TspdfNode *section_title3 = tspdf_layout_text(&ctx, "Nested Boxes & Borders", sans_bold, 18);
    section_title3->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(content, section_title3);

    TspdfNode *card_row = tspdf_layout_box(&ctx);
    card_row->width = tspdf_size_grow();
    card_row->direction = TSPDF_DIR_ROW;
    card_row->gap = 15;
    tspdf_layout_add_child(content, card_row);

    const char *card_titles[] = {"Feature", "Status", "Notes"};
    const char *card_bodies[] = {
        "TrueType font parsing with full glyph metrics and font embedding in the PDF.",
        "Flexbox layout engine with grow, fit, fixed and percentage sizing modes.",
        "All written in pure C with zero external dependencies. Only libc required."
    };
    TspdfColor card_accents[] = {
        tspdf_color_from_u8(52, 152, 219),
        tspdf_color_from_u8(46, 204, 113),
        tspdf_color_from_u8(155, 89, 182),
    };

    for (int i = 0; i < 3; i++) {
        TspdfNode *card = tspdf_layout_box(&ctx);
        card->width = tspdf_size_grow();
        card->direction = TSPDF_DIR_COLUMN;
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, card);
            s->has_background = true;
            s->background = tspdf_color_rgb(1, 1, 1);
            s->has_border = true;
            s->border_color = tspdf_color_from_u8(220, 220, 230);
            s->border_width = 1;
            s->corner_radius[0] = 8;
            s->corner_radius[1] = 8;
            s->corner_radius[2] = 8;
            s->corner_radius[3] = 8;
        }
        tspdf_layout_add_child(card_row, card);

        // Accent bar at top
        TspdfNode *accent = tspdf_layout_box(&ctx);
        accent->width = tspdf_size_grow();
        accent->height = tspdf_size_fixed(4);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, accent);
            s->has_background = true;
            s->background = card_accents[i];
            s->corner_radius[0] = 8;
            s->corner_radius[1] = 8;
        }
        tspdf_layout_add_child(card, accent);

        TspdfNode *card_content = tspdf_layout_box(&ctx);
        card_content->width = tspdf_size_grow();
        card_content->direction = TSPDF_DIR_COLUMN;
        card_content->padding = tspdf_padding_all(15);
        card_content->gap = 8;
        tspdf_layout_add_child(card, card_content);

        TspdfNode *ct = tspdf_layout_text(&ctx, card_titles[i], sans_bold, 14);
        ct->text.color = tspdf_color_from_u8(30, 30, 46);
        ct->width = tspdf_size_grow();
        tspdf_layout_add_child(card_content, ct);

        TspdfNode *cb = tspdf_layout_text(&ctx, card_bodies[i], sans, 10);
        cb->text.color = tspdf_color_from_u8(80, 80, 100);
        cb->width = tspdf_size_grow();
        cb->text.wrap = TSPDF_WRAP_WORD;
        tspdf_layout_add_child(card_content, cb);
    }

    // Section: Code sample
    TspdfNode *section_title4 = tspdf_layout_text(&ctx, "Monospace / Code", sans_bold, 18);
    section_title4->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(content, section_title4);

    if (mono) {
        TspdfNode *code_box = tspdf_layout_box(&ctx);
        code_box->width = tspdf_size_grow();
        code_box->padding = tspdf_padding_all(15);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, code_box);
            s->has_background = true;
            s->background = tspdf_color_from_u8(30, 30, 46);
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        tspdf_layout_add_child(content, code_box);

        TspdfNode *code = tspdf_layout_text(&ctx,
            "TspdfWriter doc = tspdf_writer_create();\n"
            "const char *font = tspdf_writer_add_ttf_font(doc, \"font.ttf\");\n"
            "TspdfStream *page = tspdf_writer_add_page(&doc);\n"
            "// ... draw stuff ...\n"
            "tspdf_writer_save(doc, \"output.pdf\");",
            mono, 9);
        code->text.color = tspdf_color_from_u8(166, 227, 161);
        code->text.wrap = TSPDF_WRAP_NONE;
        code->width = tspdf_size_grow();
        tspdf_layout_add_child(code_box, code);
    }

    // --- Footer ---
    TspdfNode *footer = tspdf_layout_box(&ctx);
    footer->width = tspdf_size_grow();
    footer->height = tspdf_size_fixed(35);
    footer->direction = TSPDF_DIR_ROW;
    footer->align_x = TSPDF_ALIGN_CENTER;
    footer->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, footer);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root, footer);

    TspdfNode *footer_text = tspdf_layout_text(&ctx, "Generated by tspdf — PDF toolkit in pure C", sans, 9);
    footer_text->text.color = tspdf_color_from_u8(140, 140, 160);
    tspdf_layout_add_child(footer, footer_text);

    // Compute layout and render
    tspdf_layout_compute(&ctx, root, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root, page1);

    // ==========================================
    // PAGE 2: More layout demos
    // ==========================================
    tspdf_layout_tree_free(root);
    tspdf_arena_reset(&layout_arena);

    TspdfNode *root2 = tspdf_layout_box(&ctx);
    root2->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root2->height = tspdf_size_fit();  // natural height — will be paginated
    root2->direction = TSPDF_DIR_COLUMN;
    root2->padding = tspdf_padding_all(30);
    root2->gap = 20;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root2);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    TspdfNode *p2_title = tspdf_layout_text(&ctx, "Page 2: Sizing Modes", sans_bold, 24);
    p2_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root2, p2_title);

    // Percentage-based columns
    TspdfNode *pct_label = tspdf_layout_text(&ctx, "Percentage widths: 25% / 50% / 25%", sans, 12);
    pct_label->text.color = tspdf_color_from_u8(100, 100, 120);
    tspdf_layout_add_child(root2, pct_label);

    TspdfNode *pct_row = tspdf_layout_box(&ctx);
    pct_row->width = tspdf_size_grow();
    pct_row->height = tspdf_size_fixed(50);
    pct_row->direction = TSPDF_DIR_ROW;
    pct_row->gap = 10;
    tspdf_layout_add_child(root2, pct_row);

    double pcts[] = {0.25, 0.50, 0.25};
    TspdfColor pct_colors[] = {
        tspdf_color_from_u8(255, 107, 107),
        tspdf_color_from_u8(78, 205, 196),
        tspdf_color_from_u8(255, 230, 109),
    };
    for (int i = 0; i < 3; i++) {
        TspdfNode *b = tspdf_layout_box(&ctx);
        b->width = tspdf_size_percent(pcts[i]);
        b->height = tspdf_size_grow();
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, b);
            s->has_background = true;
            s->background = pct_colors[i];
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        b->align_x = TSPDF_ALIGN_CENTER;
        b->align_y = TSPDF_ALIGN_CENTER;
        tspdf_layout_add_child(pct_row, b);

        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f%%", pcts[i] * 100);
        TspdfNode *t = tspdf_layout_text(&ctx, lbl, sans_bold, 16);
        t->text.color = tspdf_color_from_u8(30, 30, 46);
        tspdf_layout_add_child(b, t);
    }

    // Grow with weights
    TspdfNode *grow_label = tspdf_layout_text(&ctx, "Grow with weights: 1x / 2x / 1x", sans, 12);
    grow_label->text.color = tspdf_color_from_u8(100, 100, 120);
    tspdf_layout_add_child(root2, grow_label);

    TspdfNode *grow_row = tspdf_layout_box(&ctx);
    grow_row->width = tspdf_size_grow();
    grow_row->height = tspdf_size_fixed(50);
    grow_row->direction = TSPDF_DIR_ROW;
    grow_row->gap = 10;
    tspdf_layout_add_child(root2, grow_row);

    double weights[] = {1, 2, 1};
    const char *weight_labels[] = {"1x", "2x", "1x"};
    for (int i = 0; i < 3; i++) {
        TspdfNode *b = tspdf_layout_box(&ctx);
        b->width = tspdf_size_grow_weight(weights[i]);
        b->height = tspdf_size_grow();
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, b);
            s->has_background = true;
            s->background = tspdf_color_from_u8(99, 110, 114);
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        b->align_x = TSPDF_ALIGN_CENTER;
        b->align_y = TSPDF_ALIGN_CENTER;
        tspdf_layout_add_child(grow_row, b);

        TspdfNode *t = tspdf_layout_text(&ctx, weight_labels[i], sans_bold, 16);
        t->text.color = tspdf_color_rgb(1, 1, 1);
        tspdf_layout_add_child(b, t);
    }

    // Fixed + grow
    TspdfNode *mixed_label = tspdf_layout_text(&ctx, "Mixed: Fixed(100) / Grow / Fixed(150)", sans, 12);
    mixed_label->text.color = tspdf_color_from_u8(100, 100, 120);
    tspdf_layout_add_child(root2, mixed_label);

    TspdfNode *mixed_row = tspdf_layout_box(&ctx);
    mixed_row->width = tspdf_size_grow();
    mixed_row->height = tspdf_size_fixed(50);
    mixed_row->direction = TSPDF_DIR_ROW;
    mixed_row->gap = 10;
    tspdf_layout_add_child(root2, mixed_row);

    TspdfNode *fixed_left = tspdf_layout_box(&ctx);
    fixed_left->width = tspdf_size_fixed(100);
    fixed_left->height = tspdf_size_grow();
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, fixed_left);
        s->has_background = true;
        s->background = tspdf_color_from_u8(162, 155, 254);
        s->corner_radius[0] = 6;
        s->corner_radius[1] = 6;
        s->corner_radius[2] = 6;
        s->corner_radius[3] = 6;
    }
    fixed_left->align_x = TSPDF_ALIGN_CENTER;
    fixed_left->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(mixed_row, fixed_left);
    TspdfNode *fl_t = tspdf_layout_text(&ctx, "100pt", sans_bold, 12);
    fl_t->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(fixed_left, fl_t);

    TspdfNode *grow_mid = tspdf_layout_box(&ctx);
    grow_mid->width = tspdf_size_grow();
    grow_mid->height = tspdf_size_grow();
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, grow_mid);
        s->has_background = true;
        s->background = tspdf_color_from_u8(116, 185, 255);
        s->corner_radius[0] = 6;
        s->corner_radius[1] = 6;
        s->corner_radius[2] = 6;
        s->corner_radius[3] = 6;
    }
    grow_mid->align_x = TSPDF_ALIGN_CENTER;
    grow_mid->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(mixed_row, grow_mid);
    TspdfNode *gm_t = tspdf_layout_text(&ctx, "grow", sans_bold, 12);
    gm_t->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(grow_mid, gm_t);

    TspdfNode *fixed_right = tspdf_layout_box(&ctx);
    fixed_right->width = tspdf_size_fixed(150);
    fixed_right->height = tspdf_size_grow();
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, fixed_right);
        s->has_background = true;
        s->background = tspdf_color_from_u8(253, 121, 168);
        s->corner_radius[0] = 6;
        s->corner_radius[1] = 6;
        s->corner_radius[2] = 6;
        s->corner_radius[3] = 6;
    }
    fixed_right->align_x = TSPDF_ALIGN_CENTER;
    fixed_right->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(mixed_row, fixed_right);
    TspdfNode *fr_t = tspdf_layout_text(&ctx, "150pt", sans_bold, 12);
    fr_t->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(fixed_right, fr_t);

    // Long text wrapping demo
    TspdfNode *wrap_title = tspdf_layout_text(&ctx, "Long Text with Word Wrapping", sans_bold, 18);
    wrap_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root2, wrap_title);

    TspdfNode *wrap_box = tspdf_layout_box(&ctx);
    wrap_box->width = tspdf_size_grow();
    wrap_box->padding = tspdf_padding_all(20);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, wrap_box);
        s->has_background = true;
        s->background = tspdf_color_from_u8(250, 250, 255);
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(200, 200, 220);
        s->border_width = 1;
        s->corner_radius[0] = 8;
        s->corner_radius[1] = 8;
        s->corner_radius[2] = 8;
        s->corner_radius[3] = 8;
    }
    tspdf_layout_add_child(root2, wrap_box);

    TspdfNode *long_text = tspdf_layout_text(&ctx,
        "This PDF was generated entirely from scratch in C, with zero external dependencies. "
        "The layout engine implements a flexbox-inspired model with support for fixed, grow, "
        "fit-content, and percentage-based sizing. Text is measured using real TrueType font "
        "metrics parsed directly from .ttf files, which are then embedded into the PDF for "
        "accurate rendering. The word wrapping algorithm breaks text at word boundaries to "
        "fit within the available width, handling multi-line paragraphs naturally.",
        sans, 11);
    long_text->text.color = tspdf_color_from_u8(50, 50, 70);
    long_text->width = tspdf_size_grow();
    long_text->text.wrap = TSPDF_WRAP_WORD;
    long_text->text.line_height_factor = 1.5;
    tspdf_layout_add_child(wrap_box, long_text);

    // Section: Table helper demo
    TspdfNode *tbl_title = tspdf_layout_text(&ctx, "Table Helper", sans_bold, 18);
    tbl_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root2, tbl_title);

    TspdfTableStyle tbl_style = {
        .font_name = sans,
        .font_size = 10,
        .text_color = tspdf_color_from_u8(50, 50, 70),
        .header_bg = tspdf_color_from_u8(30, 30, 46),
        .header_text_color = tspdf_color_rgb(1, 1, 1),
        .row_bg_even = tspdf_color_from_u8(248, 248, 252),
        .row_bg_odd = tspdf_color_rgb(1, 1, 1),
        .border_color = tspdf_color_from_u8(200, 200, 220),
        .border_width = 1,
        .row_height = 26,
        .header_height = 30,
        .padding = 8,
    };

    const char *tbl_cols[] = {"Language", "Type", "Year"};
    double tbl_widths[] = {0.40, 0.35, 0.25};
    TspdfNode *tbl = tspdf_layout_table(&ctx, tbl_cols, tbl_widths, 3, tbl_style);
    tspdf_layout_add_child(root2, tbl);

    const char *r1[] = {"C", "Compiled", "1972"};
    const char *r2[] = {"Python", "Interpreted", "1991"};
    const char *r3[] = {"Rust", "Compiled", "2015"};
    tspdf_layout_table_add_row(&ctx, tbl, r1, tbl_widths, 3, tbl_style);
    tspdf_layout_table_add_row(&ctx, tbl, r2, tbl_widths, 3, tbl_style);
    tspdf_layout_table_add_row(&ctx, tbl, r3, tbl_widths, 3, tbl_style);

    // Section: Auto-sized table
    TspdfNode *auto_tbl_title = tspdf_layout_text(&ctx, "Auto-Sized Table (column widths computed from content)", sans_bold, 18);
    auto_tbl_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root2, auto_tbl_title);

    const char *auto_headers[] = {"ID", "Name", "Role", "Location", "Score"};
    const char *auto_rows[] = {
        "1",   "Alice",   "Engineer",       "San Francisco", "98.5",
        "2",   "Bob",     "Designer",       "NYC",           "87.2",
        "3",   "Charlie", "Product Manager", "London",       "91.0",
        "4",   "Diana",   "QA Lead",        "Berlin",        "95.3",
    };
    TspdfNode *auto_tbl = tspdf_layout_table_auto(&ctx, auto_headers, auto_rows, 4, 5, tbl_style);
    tspdf_layout_add_child(root2, auto_tbl);

    // Section: List helper demo
    TspdfNode *list_title = tspdf_layout_text(&ctx, "List Helper", sans_bold, 18);
    list_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root2, list_title);

    TspdfNode *lists_row = tspdf_layout_box(&ctx);
    lists_row->width = tspdf_size_grow();
    lists_row->direction = TSPDF_DIR_ROW;
    lists_row->gap = 30;
    tspdf_layout_add_child(root2, lists_row);

    TspdfNode *bullet_list = tspdf_layout_list(&ctx, TSPDF_LIST_BULLET, sans, 10, tspdf_color_from_u8(50, 50, 70));
    tspdf_layout_add_child(lists_row, bullet_list);
    tspdf_layout_list_add_item(&ctx, bullet_list, "Zero external dependencies");
    tspdf_layout_list_add_item(&ctx, bullet_list, "TrueType font embedding");
    tspdf_layout_list_add_item(&ctx, bullet_list, "Flexbox-style layout engine");

    TspdfNode *num_list = tspdf_layout_list(&ctx, TSPDF_LIST_NUMBERED, sans, 10, tspdf_color_from_u8(50, 50, 70));
    tspdf_layout_add_child(lists_row, num_list);
    tspdf_layout_list_add_item(&ctx, num_list, "Create a document");
    tspdf_layout_list_add_item(&ctx, num_list, "Add fonts and pages");
    tspdf_layout_list_add_item(&ctx, num_list, "Build layout tree");
    tspdf_layout_list_add_item(&ctx, num_list, "Render to PDF");

    // Paginate page 2 — auto-splits if content overflows
    TspdfPaginationResult p2_pagination;
    int p2_pages = tspdf_layout_compute_paginated(&ctx, root2, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT, &p2_pagination);
    for (int p = 0; p < p2_pages; p++) {
        TspdfStream *page2 = tspdf_writer_add_page(doc);
        tspdf_layout_render_page(&ctx, root2, &p2_pagination, p, page2);
    }

    // ==========================================
    // PAGE 3: New Features Showcase
    // ==========================================
    tspdf_layout_tree_free(root2);
    tspdf_arena_reset(&layout_arena);

    // Set document metadata
    tspdf_writer_set_title(doc, "tspdf Feature Showcase");
    tspdf_writer_set_author(doc, "tspdf");
    tspdf_writer_set_subject(doc, "Demonstrating all PDF generation features");
    tspdf_writer_set_creator(doc, "tspdf");
    tspdf_writer_set_creation_date(doc, "D:20260318120000");

    // Track page indices (page 1 = index 0, page 2 may span multiple pages)
    int pg_features = 1 + p2_pages;  // after page 1 + page 2's pages

    // Add bookmarks for navigation
    int bm_page1 = tspdf_writer_add_bookmark(doc, "Layout Engine Demo", 0);
    int bm_page2 = tspdf_writer_add_bookmark(doc, "Sizing Modes", 1);
    int bm_page3 = tspdf_writer_add_bookmark(doc, "New Features", pg_features);
    tspdf_writer_add_child_bookmark(doc, bm_page1, "Color Boxes", 0);
    tspdf_writer_add_child_bookmark(doc, bm_page1, "Text Wrapping", 0);
    tspdf_writer_add_child_bookmark(doc, bm_page1, "Cards", 0);
    tspdf_writer_add_child_bookmark(doc, bm_page3, "Opacity Demo", pg_features);
    tspdf_writer_add_child_bookmark(doc, bm_page3, "Image Embedding", pg_features);
    tspdf_writer_add_child_bookmark(doc, bm_page3, "Hyperlinks", pg_features);
    int pg_transforms = pg_features + 1;
    int bm_page4 = tspdf_writer_add_bookmark(doc, "Transforms & More", pg_transforms);
    tspdf_writer_add_child_bookmark(doc, bm_page4, "Rotation & Scale", pg_transforms);
    tspdf_writer_add_child_bookmark(doc, bm_page4, "Gradients", pg_transforms);
    tspdf_writer_add_child_bookmark(doc, bm_page4, "Clipping", pg_transforms);
    (void)bm_page2;

    // Register a linear gradient (coordinates set after layout, see below)
    // We'll use placeholder coords and update the gradient name usage later
    const char *gradient1 = tspdf_writer_add_gradient(doc,
        30, 100, 565, 100,  // horizontal, left to right (will clip to rect)
        tspdf_color_from_u8(52, 152, 219),   // blue
        tspdf_color_from_u8(155, 89, 182));  // purple

    // Register opacity states
    const char *opacity_75 = tspdf_writer_add_opacity(doc, 0.75, 0.75);
    const char *opacity_50 = tspdf_writer_add_opacity(doc, 0.50, 0.50);
    const char *opacity_25 = tspdf_writer_add_opacity(doc, 0.25, 0.25);

    // Load JPEG image
    const char *img_name = tspdf_writer_add_jpeg_image(doc,
        "examples/test.jpg");

    TspdfStream *page3 = tspdf_writer_add_page(doc);

    // --- Page 3 layout ---
    TspdfNode *root3 = tspdf_layout_box(&ctx);
    root3->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root3->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root3->direction = TSPDF_DIR_COLUMN;
    root3->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root3);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    // Header
    TspdfNode *p3_header = tspdf_layout_box(&ctx);
    p3_header->width = tspdf_size_grow();
    p3_header->height = tspdf_size_fixed(60);
    p3_header->direction = TSPDF_DIR_ROW;
    p3_header->align_y = TSPDF_ALIGN_CENTER;
    p3_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p3_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root3, p3_header);

    TspdfNode *p3_title = tspdf_layout_text(&ctx, "Page 3: New Features", sans_bold, 22);
    p3_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p3_header, p3_title);

    // Content area
    TspdfNode *p3_content = tspdf_layout_box(&ctx);
    p3_content->width = tspdf_size_grow();
    p3_content->height = tspdf_size_grow();
    p3_content->direction = TSPDF_DIR_COLUMN;
    p3_content->padding = tspdf_padding_all(30);
    p3_content->gap = 15;
    tspdf_layout_add_child(root3, p3_content);

    // --- Section 1: Opacity Demo ---
    TspdfNode *opacity_title = tspdf_layout_text(&ctx, "Opacity / Transparency", sans_bold, 16);
    opacity_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p3_content, opacity_title);

    TspdfNode *opacity_desc = tspdf_layout_text(&ctx,
        "ExtGState with ca/CA operators for fill and stroke transparency.",
        sans, 10);
    opacity_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    opacity_desc->width = tspdf_size_grow();
    opacity_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p3_content, opacity_desc);

    // Compute layout first so we know positions, then draw opacity rectangles manually
    // We'll use a placeholder row for spacing
    TspdfNode *opacity_row = tspdf_layout_box(&ctx);
    opacity_row->width = tspdf_size_grow();
    opacity_row->height = tspdf_size_fixed(80);
    tspdf_layout_add_child(p3_content, opacity_row);

    // --- Section 2: JPEG Image Embedding ---
    TspdfNode *img_title = tspdf_layout_text(&ctx, "JPEG Image Embedding", sans_bold, 16);
    img_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p3_content, img_title);

    TspdfNode *img_desc = tspdf_layout_text(&ctx,
        "JPEG files are embedded directly using DCTDecode - no decoding needed. "
        "We just scan the SOF marker for dimensions and pass the raw bytes through.",
        sans, 10);
    img_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    img_desc->width = tspdf_size_grow();
    img_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p3_content, img_desc);

    TspdfNode *img_placeholder = tspdf_layout_box(&ctx);
    img_placeholder->width = tspdf_size_grow();
    img_placeholder->height = tspdf_size_fixed(160);
    tspdf_layout_add_child(p3_content, img_placeholder);

    // --- Section 3: Hyperlinks ---
    TspdfNode *link_title = tspdf_layout_text(&ctx, "Hyperlinks & Annotations", sans_bold, 16);
    link_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p3_content, link_title);

    TspdfNode *link_row = tspdf_layout_box(&ctx);
    link_row->width = tspdf_size_grow();
    link_row->height = tspdf_size_fixed(40);
    link_row->direction = TSPDF_DIR_ROW;
    link_row->gap = 20;
    link_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p3_content, link_row);

    TspdfNode *link1 = tspdf_layout_text(&ctx, "github.com/anthropics", sans, 12);
    link1->text.color = tspdf_color_from_u8(52, 152, 219);
    tspdf_layout_add_child(link_row, link1);

    TspdfNode *link2 = tspdf_layout_text(&ctx, "en.wikipedia.org/wiki/PDF", sans, 12);
    link2->text.color = tspdf_color_from_u8(52, 152, 219);
    tspdf_layout_add_child(link_row, link2);

    // --- Section 4: TSPDF_WRAP_CHAR demo ---
    TspdfNode *char_wrap_title = tspdf_layout_text(&ctx, "Character-Level Wrapping (TSPDF_WRAP_CHAR)", sans_bold, 16);
    char_wrap_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p3_content, char_wrap_title);

    TspdfNode *char_wrap_row = tspdf_layout_box(&ctx);
    char_wrap_row->width = tspdf_size_grow();
    char_wrap_row->direction = TSPDF_DIR_ROW;
    char_wrap_row->gap = 15;
    tspdf_layout_add_child(p3_content, char_wrap_row);

    // TSPDF_WRAP_WORD vs TSPDF_WRAP_CHAR comparison
    const char *wrap_labels[] = {"TSPDF_WRAP_WORD", "TSPDF_WRAP_CHAR"};
    TspdfWrapMode wrap_modes[] = {TSPDF_WRAP_WORD, TSPDF_WRAP_CHAR};
    const char *long_word_text = "Superlongunbreakablewordthatcannotfitinacolumn plus normal words mixed in.";

    for (int i = 0; i < 2; i++) {
        TspdfNode *wrap_col = tspdf_layout_box(&ctx);
        wrap_col->width = tspdf_size_grow();
        wrap_col->direction = TSPDF_DIR_COLUMN;
        wrap_col->gap = 5;
        wrap_col->padding = tspdf_padding_all(12);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, wrap_col);
            s->has_background = true;
            s->background = tspdf_color_from_u8(248, 248, 255);
            s->has_border = true;
            s->border_color = tspdf_color_from_u8(200, 200, 220);
            s->border_width = 1;
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        tspdf_layout_add_child(char_wrap_row, wrap_col);

        TspdfNode *wl = tspdf_layout_text(&ctx, wrap_labels[i], sans_bold, 10);
        wl->text.color = tspdf_color_from_u8(100, 100, 130);
        wl->width = tspdf_size_grow();
        wl->text.alignment = TSPDF_TEXT_ALIGN_CENTER;
        tspdf_layout_add_child(wrap_col, wl);

        TspdfNode *wt = tspdf_layout_text(&ctx, long_word_text, sans, 10);
        wt->text.color = tspdf_color_from_u8(50, 50, 70);
        wt->width = tspdf_size_grow();
        wt->text.wrap = wrap_modes[i];
        tspdf_layout_add_child(wrap_col, wt);
    }

    // --- Section 5: Metadata & Bookmarks info ---
    TspdfNode *meta_title = tspdf_layout_text(&ctx, "Metadata & Bookmarks", sans_bold, 16);
    meta_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p3_content, meta_title);

    TspdfNode *meta_desc = tspdf_layout_text(&ctx,
        "This PDF has document metadata (title, author, subject, creator, creation date) "
        "and a bookmark outline tree visible in the sidebar. Check your PDF viewer's "
        "document properties and bookmarks panel.",
        sans, 10);
    meta_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    meta_desc->width = tspdf_size_grow();
    meta_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p3_content, meta_desc);

    // Compute layout
    tspdf_layout_compute(&ctx, root3, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root3, page3);

    // --- Draw opacity rectangles manually (after layout render, on top) ---
    // These use the opacity_row's computed position for placement
    double opacity_base_x = opacity_row->computed_x + 10;
    double opacity_base_y_layout = opacity_row->computed_y + 5;
    // Convert to PDF coords (y-up)
    double page_h = TSPDF_PAGE_A4_HEIGHT;

    TspdfColor opacity_colors[] = {
        tspdf_color_from_u8(231, 76, 60),   // red
        tspdf_color_from_u8(46, 204, 113),  // green
        tspdf_color_from_u8(52, 152, 219),  // blue
    };
    const char *gs_names[] = {opacity_75, opacity_50, opacity_25};
    const char *opacity_labels[] = {"75%", "50%", "25%"};

    for (int i = 0; i < 3; i++) {
        double rx = opacity_base_x + i * 110;
        double ry = page_h - opacity_base_y_layout - 70;  // PDF y-up

        // Full opacity background
        tspdf_stream_save(page3);
        tspdf_stream_set_fill_color(page3, tspdf_color_from_u8(200, 200, 210));
        tspdf_stream_rect(page3, rx, ry, 90, 70);
        tspdf_stream_fill(page3);
        tspdf_stream_restore(page3);

        // Overlapping semi-transparent rectangle
        tspdf_stream_save(page3);
        if (gs_names[i]) tspdf_stream_set_opacity(page3, gs_names[i]);
        tspdf_stream_set_fill_color(page3, opacity_colors[i]);
        tspdf_stream_rect(page3, rx + 15, ry - 10, 90, 70);
        tspdf_stream_fill(page3);
        tspdf_stream_restore(page3);

        // Label
        tspdf_stream_save(page3);
        tspdf_stream_begin_text(page3);
        tspdf_stream_set_font(page3, sans, 10);
        tspdf_stream_set_fill_color(page3, tspdf_color_from_u8(30, 30, 46));
        tspdf_stream_text_position(page3, rx + 30, ry - 20);
        tspdf_stream_show_text(page3, opacity_labels[i]);
        tspdf_stream_end_text(page3);
        tspdf_stream_restore(page3);
    }

    // --- Draw JPEG image manually ---
    if (img_name) {
        double img_x = img_placeholder->computed_x + 10;
        double img_y_layout = img_placeholder->computed_y;
        double img_pdf_y = page_h - img_y_layout - 150;
        tspdf_stream_draw_image(page3, img_name, img_x, img_pdf_y, 200, 150);

        // Caption next to image
        tspdf_stream_save(page3);
        tspdf_stream_begin_text(page3);
        tspdf_stream_set_font(page3, sans, 10);
        tspdf_stream_set_fill_color(page3, tspdf_color_from_u8(80, 80, 100));
        tspdf_stream_text_position(page3, img_x + 220, img_pdf_y + 130);
        tspdf_stream_show_text(page3, "test.jpg");
        tspdf_stream_end_text(page3);

        tspdf_stream_begin_text(page3);
        tspdf_stream_set_font(page3, sans, 9);
        tspdf_stream_set_fill_color(page3, tspdf_color_from_u8(120, 120, 140));
        tspdf_stream_text_position(page3, img_x + 220, img_pdf_y + 115);
        tspdf_stream_show_text(page3, "Embedded via DCTDecode (raw JPEG passthrough)");
        tspdf_stream_end_text(page3);
        tspdf_stream_restore(page3);
    }

    // --- Add hyperlink annotations (PDF coordinates) ---
    // We need to use link1 and link2's computed positions
    {
        double lx1 = link1->computed_x;
        double ly1_layout = link1->computed_y;
        double lw1 = link1->computed_width;
        double lh1 = link1->computed_height;
        double ly1_pdf = page_h - ly1_layout - lh1;
        tspdf_writer_add_link(doc, 2, lx1, ly1_pdf, lw1, lh1,
            "https://github.com/anthropics");

        double lx2 = link2->computed_x;
        double ly2_layout = link2->computed_y;
        double lw2 = link2->computed_width;
        double lh2 = link2->computed_height;
        double ly2_pdf = page_h - ly2_layout - lh2;
        tspdf_writer_add_link(doc, 2, lx2, ly2_pdf, lw2, lh2,
            "https://en.wikipedia.org/wiki/Portable_Document_Format");
    }

    // ==========================================
    // ==========================================
    // PAGE 4: Transforms, Gradients, Clipping
    // ==========================================
    tspdf_layout_tree_free(root3);
    tspdf_arena_reset(&layout_arena);

    TspdfStream *page4 = tspdf_writer_add_page(doc);

    TspdfNode *root4 = tspdf_layout_box(&ctx);
    root4->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root4->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root4->direction = TSPDF_DIR_COLUMN;
    root4->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root4);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    TspdfNode *p4_header = tspdf_layout_box(&ctx);
    p4_header->width = tspdf_size_grow();
    p4_header->height = tspdf_size_fixed(60);
    p4_header->direction = TSPDF_DIR_ROW;
    p4_header->align_y = TSPDF_ALIGN_CENTER;
    p4_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p4_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root4, p4_header);

    TspdfNode *p4_title = tspdf_layout_text(&ctx, "Page 4: Transforms & More", sans_bold, 22);
    p4_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p4_header, p4_title);

    TspdfNode *p4_content = tspdf_layout_box(&ctx);
    p4_content->width = tspdf_size_grow();
    p4_content->height = tspdf_size_grow();
    p4_content->direction = TSPDF_DIR_COLUMN;
    p4_content->padding = tspdf_padding_all(30);
    p4_content->gap = 15;
    tspdf_layout_add_child(root4, p4_content);

    // Rotation demo
    TspdfNode *rot_title = tspdf_layout_text(&ctx, "Rotation & Scale Transforms", sans_bold, 16);
    rot_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p4_content, rot_title);

    TspdfNode *rot_desc = tspdf_layout_text(&ctx,
        "Elements can be rotated around their center and scaled using the PDF cm operator.",
        sans, 10);
    rot_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    rot_desc->width = tspdf_size_grow();
    rot_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p4_content, rot_desc);

    TspdfNode *rot_row = tspdf_layout_box(&ctx);
    rot_row->width = tspdf_size_grow();
    rot_row->height = tspdf_size_fixed(100);
    rot_row->direction = TSPDF_DIR_ROW;
    rot_row->gap = 40;
    rot_row->align_y = TSPDF_ALIGN_CENTER;
    rot_row->align_x = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p4_content, rot_row);

    double angles[] = {0, 15, 30, 45};
    for (int i = 0; i < 4; i++) {
        TspdfNode *rbox = tspdf_layout_box(&ctx);
        rbox->width = tspdf_size_fixed(55);
        rbox->height = tspdf_size_fixed(55);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, rbox);
            s->has_background = true;
            s->background = tspdf_color_from_u8(52, 152, 219);
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        rbox->align_x = TSPDF_ALIGN_CENTER;
        rbox->align_y = TSPDF_ALIGN_CENTER;
        rbox->rotation = angles[i];
        tspdf_layout_add_child(rot_row, rbox);

        char rlbl[16];
        snprintf(rlbl, sizeof(rlbl), "%.0f deg", angles[i]);
        TspdfNode *rt = tspdf_layout_text(&ctx, rlbl, sans_bold, 11);
        rt->text.color = tspdf_color_rgb(1, 1, 1);
        tspdf_layout_add_child(rbox, rt);
    }

    TspdfNode *sbox = tspdf_layout_box(&ctx);
    sbox->width = tspdf_size_fixed(55);
    sbox->height = tspdf_size_fixed(55);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, sbox);
        s->has_background = true;
        s->background = tspdf_color_from_u8(231, 76, 60);
        s->corner_radius[0] = 6;
        s->corner_radius[1] = 6;
        s->corner_radius[2] = 6;
        s->corner_radius[3] = 6;
    }
    sbox->align_x = TSPDF_ALIGN_CENTER;
    sbox->align_y = TSPDF_ALIGN_CENTER;
    sbox->scale_x = 1.4;
    sbox->scale_y = 0.6;
    tspdf_layout_add_child(rot_row, sbox);

    TspdfNode *st = tspdf_layout_text(&ctx, "scaled", sans_bold, 9);
    st->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(sbox, st);

    // Gradient demo
    TspdfNode *grad_title = tspdf_layout_text(&ctx, "Linear Gradients", sans_bold, 16);
    grad_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p4_content, grad_title);

    TspdfNode *grad_desc = tspdf_layout_text(&ctx,
        "PDF Shading Type 2 (axial) with exponential interpolation between two colors.",
        sans, 10);
    grad_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    grad_desc->width = tspdf_size_grow();
    grad_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p4_content, grad_desc);

    TspdfNode *grad_placeholder = tspdf_layout_box(&ctx);
    grad_placeholder->width = tspdf_size_grow();
    grad_placeholder->height = tspdf_size_fixed(40);
    tspdf_layout_add_child(p4_content, grad_placeholder);

    // Clipping demo
    TspdfNode *clip_title = tspdf_layout_text(&ctx, "Clipping / Scissor Region", sans_bold, 16);
    clip_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p4_content, clip_title);

    TspdfNode *clip_desc = tspdf_layout_text(&ctx,
        "Children are clipped to the parent's bounds using PDF W n operators.",
        sans, 10);
    clip_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    clip_desc->width = tspdf_size_grow();
    clip_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p4_content, clip_desc);

    TspdfNode *clip_box = tspdf_layout_box(&ctx);
    clip_box->width = tspdf_size_fixed(300);
    clip_box->height = tspdf_size_fixed(50);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, clip_box);
        s->has_background = true;
        s->background = tspdf_color_from_u8(248, 248, 255);
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(200, 200, 220);
        s->border_width = 1;
        s->corner_radius[0] = 6;
        s->corner_radius[1] = 6;
        s->corner_radius[2] = 6;
        s->corner_radius[3] = 6;
    }
    clip_box->clip_children = true;
    tspdf_layout_add_child(p4_content, clip_box);

    TspdfNode *overflow_text = tspdf_layout_text(&ctx,
        "This text is intentionally way too wide for the 300pt container and will be clipped at the boundary. "
        "Everything past the edge is invisible thanks to the PDF clipping path.",
        sans, 12);
    overflow_text->text.color = tspdf_color_from_u8(231, 76, 60);
    overflow_text->text.wrap = TSPDF_WRAP_NONE;
    overflow_text->padding = tspdf_padding_xy(10, 15);
    tspdf_layout_add_child(clip_box, overflow_text);

    // Compute and render page 4
    tspdf_layout_compute(&ctx, root4, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root4, page4);

    // Draw gradient bar manually on page 4
    if (gradient1) {
        double gx = grad_placeholder->computed_x + 10;
        double gy_layout = grad_placeholder->computed_y;
        double gy_pdf = TSPDF_PAGE_A4_HEIGHT - gy_layout - 30;
        tspdf_stream_save(page4);
        tspdf_stream_rounded_rect_pdf(page4, gx, gy_pdf, 480, 30, 6, 6, 6, 6);
        tspdf_stream_clip(page4);
        tspdf_stream_fill_gradient(page4, gradient1);
        tspdf_stream_restore(page4);

        tspdf_stream_save(page4);
        tspdf_stream_begin_text(page4);
        tspdf_stream_set_font(page4, sans_bold, 11);
        tspdf_stream_set_fill_color(page4, tspdf_color_rgb(1, 1, 1));
        tspdf_stream_text_position(page4, gx + 15, gy_pdf + 10);
        tspdf_stream_show_text(page4, "Blue to Purple gradient (Shading Type 2)");
        tspdf_stream_end_text(page4);
        tspdf_stream_restore(page4);
    }

    // ==========================================
    // PAGE 5: New Layout Features Demo
    // ==========================================
    tspdf_layout_tree_free(root4);
    tspdf_arena_reset(&layout_arena);

    TspdfStream *page5 = tspdf_writer_add_page(doc);

    TspdfNode *root5 = tspdf_layout_box(&ctx);
    root5->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root5->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root5->direction = TSPDF_DIR_COLUMN;
    root5->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root5);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    // Header
    TspdfNode *p5_header = tspdf_layout_box(&ctx);
    p5_header->width = tspdf_size_grow();
    p5_header->height = tspdf_size_fixed(60);
    p5_header->direction = TSPDF_DIR_ROW;
    p5_header->align_y = TSPDF_ALIGN_CENTER;
    p5_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p5_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root5, p5_header);

    TspdfNode *p5_title = tspdf_layout_text(&ctx, "Page 5: Per-Side Borders, Dashes, Text Decorations", sans_bold, 18);
    p5_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p5_header, p5_title);

    TspdfNode *p5_content = tspdf_layout_box(&ctx);
    p5_content->width = tspdf_size_grow();
    p5_content->height = tspdf_size_grow();
    p5_content->direction = TSPDF_DIR_COLUMN;
    p5_content->padding = tspdf_padding_all(30);
    p5_content->gap = 18;
    tspdf_layout_add_child(root5, p5_content);

    // --- Section 1: Per-side borders ---
    TspdfNode *ps_title = tspdf_layout_text(&ctx, "Per-Side Borders", sans_bold, 16);
    ps_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p5_content, ps_title);

    TspdfNode *ps_row = tspdf_layout_box(&ctx);
    ps_row->width = tspdf_size_grow();
    ps_row->height = tspdf_size_fixed(70);
    ps_row->direction = TSPDF_DIR_ROW;
    ps_row->gap = 20;
    ps_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p5_content, ps_row);

    // Box with only left border (accent)
    TspdfNode *ps1 = tspdf_layout_box(&ctx);
    ps1->width = tspdf_size_fixed(140);
    ps1->height = tspdf_size_fixed(60);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps1);
        s->has_background = true;
        s->background = tspdf_color_from_u8(240, 245, 255);
    }
    ps1->align_y = TSPDF_ALIGN_CENTER;
    ps1->padding = tspdf_padding_xy(12, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps1);
        s->has_border_left = true;
        s->border_left = 4;
        s->border_color_left = tspdf_color_from_u8(52, 152, 219);
    }
    tspdf_layout_add_child(ps_row, ps1);
    TspdfNode *ps1t = tspdf_layout_text(&ctx, "Left accent", sans, 11);
    ps1t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(ps1, ps1t);

    // Box with top and bottom borders
    TspdfNode *ps2 = tspdf_layout_box(&ctx);
    ps2->width = tspdf_size_fixed(140);
    ps2->height = tspdf_size_fixed(60);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps2);
        s->has_background = true;
        s->background = tspdf_color_from_u8(255, 248, 240);
    }
    ps2->align_x = TSPDF_ALIGN_CENTER;
    ps2->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps2);
        s->has_border_top = true;
        s->border_top = 3;
        s->border_color_top = tspdf_color_from_u8(230, 126, 34);
        s->has_border_bottom = true;
        s->border_bottom = 3;
        s->border_color_bottom = tspdf_color_from_u8(230, 126, 34);
    }
    tspdf_layout_add_child(ps_row, ps2);
    TspdfNode *ps2t = tspdf_layout_text(&ctx, "Top + Bottom", sans, 11);
    ps2t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(ps2, ps2t);

    // Box with each side a different color
    TspdfNode *ps3 = tspdf_layout_box(&ctx);
    ps3->width = tspdf_size_fixed(140);
    ps3->height = tspdf_size_fixed(60);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps3);
        s->has_background = true;
        s->background = tspdf_color_from_u8(245, 245, 250);
    }
    ps3->align_x = TSPDF_ALIGN_CENTER;
    ps3->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, ps3);
        s->has_border_top = true;
        s->border_top = 3;
        s->border_color_top = tspdf_color_from_u8(231, 76, 60);
        s->has_border_right = true;
        s->border_right = 3;
        s->border_color_right = tspdf_color_from_u8(46, 204, 113);
        s->has_border_bottom = true;
        s->border_bottom = 3;
        s->border_color_bottom = tspdf_color_from_u8(52, 152, 219);
        s->has_border_left = true;
        s->border_left = 3;
        s->border_color_left = tspdf_color_from_u8(155, 89, 182);
    }
    tspdf_layout_add_child(ps_row, ps3);
    TspdfNode *ps3t = tspdf_layout_text(&ctx, "4 colors", sans, 11);
    ps3t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(ps3, ps3t);

    // --- Section 2: Dashed / Dotted Borders ---
    TspdfNode *dash_title = tspdf_layout_text(&ctx, "Dashed & Dotted Borders", sans_bold, 16);
    dash_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p5_content, dash_title);

    TspdfNode *dash_row = tspdf_layout_box(&ctx);
    dash_row->width = tspdf_size_grow();
    dash_row->height = tspdf_size_fixed(70);
    dash_row->direction = TSPDF_DIR_ROW;
    dash_row->gap = 20;
    dash_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p5_content, dash_row);

    // Dashed border
    TspdfNode *d1 = tspdf_layout_box(&ctx);
    d1->width = tspdf_size_fixed(140);
    d1->height = tspdf_size_fixed(55);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, d1);
        s->has_border = true;
        s->border_width = 2;
        s->border_color = tspdf_color_from_u8(52, 152, 219);
        s->dash_on = 6;
        s->dash_off = 4;
    }
    d1->align_x = TSPDF_ALIGN_CENTER;
    d1->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(dash_row, d1);
    TspdfNode *d1t = tspdf_layout_text(&ctx, "Dashed (6-4)", sans, 10);
    d1t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(d1, d1t);

    // Dotted border
    TspdfNode *d2 = tspdf_layout_box(&ctx);
    d2->width = tspdf_size_fixed(140);
    d2->height = tspdf_size_fixed(55);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, d2);
        s->has_border = true;
        s->border_width = 2;
        s->border_color = tspdf_color_from_u8(231, 76, 60);
        s->dash_on = 2;
        s->dash_off = 3;
    }
    d2->align_x = TSPDF_ALIGN_CENTER;
    d2->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(dash_row, d2);
    TspdfNode *d2t = tspdf_layout_text(&ctx, "Dotted (2-3)", sans, 10);
    d2t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(d2, d2t);

    // Long dash
    TspdfNode *d3 = tspdf_layout_box(&ctx);
    d3->width = tspdf_size_fixed(140);
    d3->height = tspdf_size_fixed(55);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, d3);
        s->has_border = true;
        s->border_width = 1.5;
        s->border_color = tspdf_color_from_u8(46, 204, 113);
        s->dash_on = 12;
        s->dash_off = 6;
        s->corner_radius[0] = 8;
        s->corner_radius[1] = 8;
        s->corner_radius[2] = 8;
        s->corner_radius[3] = 8;
    }
    d3->align_x = TSPDF_ALIGN_CENTER;
    d3->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(dash_row, d3);
    TspdfNode *d3t = tspdf_layout_text(&ctx, "Rounded + dash", sans, 10);
    d3t->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(d3, d3t);

    // --- Section 3: Text Decorations ---
    TspdfNode *decor_title = tspdf_layout_text(&ctx, "Text Decorations", sans_bold, 16);
    decor_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p5_content, decor_title);

    TspdfNode *decor_col = tspdf_layout_box(&ctx);
    decor_col->width = tspdf_size_grow();
    decor_col->height = tspdf_size_fit();
    decor_col->direction = TSPDF_DIR_COLUMN;
    decor_col->gap = 8;
    tspdf_layout_add_child(p5_content, decor_col);

    TspdfNode *ul_text = tspdf_layout_text(&ctx, "This text has an underline decoration", sans, 14);
    ul_text->text.color = tspdf_color_from_u8(50, 50, 70);
    ul_text->text.decoration = TSPDF_TEXT_DECOR_UNDERLINE;
    tspdf_layout_add_child(decor_col, ul_text);

    TspdfNode *st_text = tspdf_layout_text(&ctx, "This text has strikethrough (deleted)", sans, 14);
    st_text->text.color = tspdf_color_from_u8(150, 50, 50);
    st_text->text.decoration = TSPDF_TEXT_DECOR_STRIKETHROUGH;
    tspdf_layout_add_child(decor_col, st_text);

    TspdfNode *ol_text = tspdf_layout_text(&ctx, "This text has an overline decoration", sans, 14);
    ol_text->text.color = tspdf_color_from_u8(50, 50, 150);
    ol_text->text.decoration = TSPDF_TEXT_DECOR_OVERLINE;
    tspdf_layout_add_child(decor_col, ol_text);

    TspdfNode *combo_text = tspdf_layout_text(&ctx, "Underline + Strikethrough combined!", sans_bold, 14);
    combo_text->text.color = tspdf_color_from_u8(155, 89, 182);
    combo_text->text.decoration = TSPDF_TEXT_DECOR_UNDERLINE | TSPDF_TEXT_DECOR_STRIKETHROUGH;
    tspdf_layout_add_child(decor_col, combo_text);

    // --- Section 4: Auto-Sized Table (tspdf_layout_table_auto) ---
    TspdfNode *auto_title = tspdf_layout_text(&ctx, "Auto-Sized Table Columns", sans_bold, 16);
    auto_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p5_content, auto_title);

    {
        const char *hdrs[] = {"ID", "Name", "Department", "Status"};
        const char *rows_flat[] = {
            "1", "Alice Johnson", "Engineering", "Active",
            "2", "Bob Smith", "Marketing", "On Leave",
            "3", "Charlie Brown", "Design", "Active",
            "4", "Diana Prince", "Management", "Active",
        };
        TspdfTableStyle ts = {
            .font_name = sans,
            .font_size = 10,
            .text_color = tspdf_color_from_u8(50, 50, 70),
            .header_bg = tspdf_color_from_u8(30, 30, 46),
            .header_text_color = tspdf_color_rgb(1, 1, 1),
            .row_bg_even = tspdf_color_rgb(1, 1, 1),
            .row_bg_odd = tspdf_color_from_u8(248, 248, 252),
            .border_color = tspdf_color_from_u8(220, 220, 230),
            .border_width = 0.5,
            .row_height = 0,  // auto height
            .header_height = 35,
            .padding = 8,
        };
        TspdfNode *auto_tbl = tspdf_layout_table_auto(&ctx, hdrs, rows_flat, 4, 4, ts);
        tspdf_layout_add_child(p5_content, auto_tbl);
    }

    // Compute & render page 5
    tspdf_layout_compute(&ctx, root5, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root5, page5);

    int pg_new_features = pg_transforms + 1;
    int bm_page5 = tspdf_writer_add_bookmark(doc, "Layout Features", pg_new_features);
    tspdf_writer_add_child_bookmark(doc, bm_page5, "Per-Side Borders", pg_new_features);
    tspdf_writer_add_child_bookmark(doc, bm_page5, "Dashed Borders", pg_new_features);
    tspdf_writer_add_child_bookmark(doc, bm_page5, "Text Decorations", pg_new_features);
    tspdf_writer_add_child_bookmark(doc, bm_page5, "Auto-Sized Table", pg_new_features);

    // ==========================================
    // PAGE 6: Drop Shadows & More
    // ==========================================
    tspdf_layout_tree_free(root5);
    tspdf_arena_reset(&layout_arena);

    TspdfStream *page6 = tspdf_writer_add_page(doc);

    TspdfNode *root6 = tspdf_layout_box(&ctx);
    root6->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root6->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root6->direction = TSPDF_DIR_COLUMN;
    root6->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root6);
        s->has_background = true;
        s->background = tspdf_color_from_u8(245, 245, 250);
    }

    // Header
    TspdfNode *p6_header = tspdf_layout_box(&ctx);
    p6_header->width = tspdf_size_grow();
    p6_header->height = tspdf_size_fixed(60);
    p6_header->direction = TSPDF_DIR_ROW;
    p6_header->align_y = TSPDF_ALIGN_CENTER;
    p6_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p6_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root6, p6_header);

    TspdfNode *p6_title = tspdf_layout_text(&ctx, "Page 6: Drop Shadows & Page Numbers", sans_bold, 20);
    p6_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p6_header, p6_title);

    TspdfNode *p6_content = tspdf_layout_box(&ctx);
    p6_content->width = tspdf_size_grow();
    p6_content->height = tspdf_size_grow();
    p6_content->direction = TSPDF_DIR_COLUMN;
    p6_content->padding = tspdf_padding_all(25);
    p6_content->gap = 12;
    tspdf_layout_add_child(root6, p6_content);

    // --- Drop Shadow Demo ---
    TspdfNode *sh_title = tspdf_layout_text(&ctx, "Drop Shadows", sans_bold, 16);
    sh_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, sh_title);

    TspdfNode *sh_desc = tspdf_layout_text(&ctx,
        "Boxes can have drop shadows with configurable offset and blur simulation.",
        sans, 10);
    sh_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    sh_desc->width = tspdf_size_grow();
    sh_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p6_content, sh_desc);

    TspdfNode *sh_row = tspdf_layout_box(&ctx);
    sh_row->width = tspdf_size_grow();
    sh_row->height = tspdf_size_fixed(100);
    sh_row->direction = TSPDF_DIR_ROW;
    sh_row->gap = 40;
    sh_row->align_y = TSPDF_ALIGN_CENTER;
    sh_row->align_x = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p6_content, sh_row);

    // Card with subtle shadow
    TspdfNode *card1 = tspdf_layout_box(&ctx);
    card1->width = tspdf_size_fixed(160);
    card1->height = tspdf_size_fixed(90);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, card1);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
        s->corner_radius[0] = 8;
        s->corner_radius[1] = 8;
        s->corner_radius[2] = 8;
        s->corner_radius[3] = 8;
        s->has_shadow = true;
        s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.15);
        s->shadow_offset_x = 2;
        s->shadow_offset_y = 3;
        s->shadow_blur = 6;
    }
    card1->padding = tspdf_padding_all(12);
    card1->direction = TSPDF_DIR_COLUMN;
    card1->gap = 4;
    tspdf_layout_add_child(sh_row, card1);

    TspdfNode *c1t = tspdf_layout_text(&ctx, "Subtle Shadow", sans_bold, 12);
    c1t->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(card1, c1t);
    TspdfNode *c1d = tspdf_layout_text(&ctx, "offset: 2,3  blur: 6", sans, 9);
    c1d->text.color = tspdf_color_from_u8(120, 120, 140);
    tspdf_layout_add_child(card1, c1d);

    // Card with strong shadow
    TspdfNode *card2 = tspdf_layout_box(&ctx);
    card2->width = tspdf_size_fixed(160);
    card2->height = tspdf_size_fixed(90);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, card2);
        s->has_background = true;
        s->background = tspdf_color_from_u8(52, 152, 219);
        s->corner_radius[0] = 8;
        s->corner_radius[1] = 8;
        s->corner_radius[2] = 8;
        s->corner_radius[3] = 8;
        s->has_shadow = true;
        s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.4);
        s->shadow_offset_x = 4;
        s->shadow_offset_y = 6;
        s->shadow_blur = 10;
    }
    card2->padding = tspdf_padding_all(12);
    card2->direction = TSPDF_DIR_COLUMN;
    card2->gap = 4;
    tspdf_layout_add_child(sh_row, card2);

    TspdfNode *c2t = tspdf_layout_text(&ctx, "Strong Shadow", sans_bold, 12);
    c2t->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(card2, c2t);
    TspdfNode *c2d = tspdf_layout_text(&ctx, "offset: 4,6  blur: 10", sans, 9);
    c2d->text.color = tspdf_color_from_u8(200, 220, 240);
    tspdf_layout_add_child(card2, c2d);

    // Card with no blur (hard shadow)
    TspdfNode *card3 = tspdf_layout_box(&ctx);
    card3->width = tspdf_size_fixed(160);
    card3->height = tspdf_size_fixed(90);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, card3);
        s->has_background = true;
        s->background = tspdf_color_from_u8(46, 204, 113);
        s->corner_radius[0] = 0;
        s->corner_radius[1] = 0;
        s->corner_radius[2] = 0;
        s->corner_radius[3] = 0;
        s->has_shadow = true;
        s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.3);
        s->shadow_offset_x = 5;
        s->shadow_offset_y = 5;
        s->shadow_blur = 0;
    }
    card3->padding = tspdf_padding_all(12);
    card3->direction = TSPDF_DIR_COLUMN;
    card3->gap = 4;
    tspdf_layout_add_child(sh_row, card3);

    TspdfNode *c3t = tspdf_layout_text(&ctx, "Hard Shadow", sans_bold, 12);
    c3t->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(card3, c3t);
    TspdfNode *c3d = tspdf_layout_text(&ctx, "offset: 5,5  blur: 0", sans, 9);
    c3d->text.color = tspdf_color_from_u8(200, 240, 220);
    tspdf_layout_add_child(card3, c3d);

    // --- Background Image ---
    TspdfNode *bi_title = tspdf_layout_text(&ctx, "Background Image on Box", sans_bold, 16);
    bi_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, bi_title);

    TspdfNode *bi_row = tspdf_layout_box(&ctx);
    bi_row->width = tspdf_size_grow();
    bi_row->height = tspdf_size_fixed(80);
    bi_row->direction = TSPDF_DIR_ROW;
    bi_row->gap = 20;
    bi_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p6_content, bi_row);

    // Box with background image (clipped to rounded corners)
    if (img_name) {
        TspdfNode *bi1 = tspdf_layout_box(&ctx);
        bi1->width = tspdf_size_fixed(150);
        bi1->height = tspdf_size_fixed(90);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, bi1);
            s->corner_radius[0] = 12;
            s->corner_radius[1] = 12;
            s->corner_radius[2] = 12;
            s->corner_radius[3] = 12;
            s->bg_image = img_name;
            s->has_shadow = true;
            s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.2);
            s->shadow_offset_x = 3;
            s->shadow_offset_y = 4;
            s->shadow_blur = 5;
        }
        tspdf_layout_add_child(bi_row, bi1);

        TspdfNode *bi2 = tspdf_layout_box(&ctx);
        bi2->width = tspdf_size_fixed(200);
        bi2->height = tspdf_size_fixed(90);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, bi2);
            s->bg_image = img_name;
            s->has_border = true;
            s->border_width = 2;
            s->border_color = tspdf_color_from_u8(52, 152, 219);
        }
        bi2->align_x = TSPDF_ALIGN_CENTER;
        bi2->align_y = TSPDF_ALIGN_END;
        bi2->padding = tspdf_padding_all(6);
        tspdf_layout_add_child(bi_row, bi2);

        TspdfNode *bi2t = tspdf_layout_text(&ctx, "Image with text overlay", sans_bold, 10);
        bi2t->text.color = tspdf_color_rgb(1, 1, 1);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, bi2t);
            s->has_shadow = true;
            s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.5);
            s->shadow_offset_x = 1;
            s->shadow_offset_y = 1;
            s->shadow_blur = 0;
        }
        tspdf_layout_add_child(bi2, bi2t);
    }

    // --- PNG Image Support ---
    TspdfNode *png_title = tspdf_layout_text(&ctx, "PNG Image Support (Decoded from Scratch)", sans_bold, 16);
    png_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, png_title);

    const char *png_rgb = tspdf_writer_add_png_image(doc,
        "examples/test.png");

    TspdfNode *png_row = tspdf_layout_box(&ctx);
    png_row->width = tspdf_size_grow();
    png_row->height = tspdf_size_fixed(70);
    png_row->direction = TSPDF_DIR_ROW;
    png_row->gap = 15;
    png_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p6_content, png_row);

    if (png_rgb) {
        // Circular avatar with PNG
        TspdfNode *png_box = tspdf_layout_box(&ctx);
        png_box->width = tspdf_size_fixed(60);
        png_box->height = tspdf_size_fixed(60);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, png_box);
            s->bg_image = png_rgb;
            s->corner_radius[0] = 30;
            s->corner_radius[1] = 30;
            s->corner_radius[2] = 30;
            s->corner_radius[3] = 30;
            s->has_shadow = true;
            s->shadow_color = tspdf_color_rgba(0, 0, 0, 0.2);
            s->shadow_offset_x = 2;
            s->shadow_offset_y = 3;
            s->shadow_blur = 4;
        }
        tspdf_layout_add_child(png_row, png_box);

        TspdfNode *png_info = tspdf_layout_box(&ctx);
        png_info->width = tspdf_size_grow();
        png_info->height = tspdf_size_fit();
        png_info->direction = TSPDF_DIR_COLUMN;
        png_info->gap = 3;
        tspdf_layout_add_child(png_row, png_info);

        TspdfNode *pi1 = tspdf_layout_text(&ctx, "PNG decoded from scratch (inflate + unfilter)", sans_bold, 11);
        pi1->text.color = tspdf_color_from_u8(30, 30, 46);
        tspdf_layout_add_child(png_info, pi1);

        TspdfNode *pi2 = tspdf_layout_text(&ctx, "Supports RGB and RGBA with alpha soft mask. "
            "Clipped to circle with rounded corners.", sans, 9);
        pi2->text.color = tspdf_color_from_u8(100, 100, 120);
        pi2->width = tspdf_size_grow();
        pi2->text.wrap = TSPDF_WRAP_WORD;
        tspdf_layout_add_child(png_info, pi2);
    }

    // --- Multi-Stop & Radial Gradients ---
    TspdfNode *gr_title = tspdf_layout_text(&ctx, "Multi-Stop & Radial Gradients", sans_bold, 16);
    gr_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, gr_title);

    TspdfNode *gr_row = tspdf_layout_box(&ctx);
    gr_row->width = tspdf_size_grow();
    gr_row->height = tspdf_size_fixed(50);
    gr_row->direction = TSPDF_DIR_ROW;
    gr_row->gap = 15;
    tspdf_layout_add_child(p6_content, gr_row);

    // Placeholder boxes for gradient drawing (we draw gradients manually after layout)
    TspdfNode *grad_multi = tspdf_layout_box(&ctx);
    grad_multi->width = tspdf_size_grow();
    grad_multi->height = tspdf_size_fixed(40);
    tspdf_layout_add_child(gr_row, grad_multi);

    TspdfNode *grad_radial_box = tspdf_layout_box(&ctx);
    grad_radial_box->width = tspdf_size_fixed(120);
    grad_radial_box->height = tspdf_size_fixed(40);
    tspdf_layout_add_child(gr_row, grad_radial_box);

    TspdfNode *gr_labels = tspdf_layout_box(&ctx);
    gr_labels->width = tspdf_size_grow();
    gr_labels->height = tspdf_size_fit();
    gr_labels->direction = TSPDF_DIR_ROW;
    gr_labels->gap = 15;
    tspdf_layout_add_child(p6_content, gr_labels);

    TspdfNode *gl1 = tspdf_layout_text(&ctx, "Multi-stop linear (rainbow)", sans, 9);
    gl1->text.color = tspdf_color_from_u8(100, 100, 120);
    gl1->width = tspdf_size_grow();
    tspdf_layout_add_child(gr_labels, gl1);
    TspdfNode *gl2 = tspdf_layout_text(&ctx, "Radial", sans, 9);
    gl2->text.color = tspdf_color_from_u8(100, 100, 120);
    gl2->width = tspdf_size_fixed(120);
    tspdf_layout_add_child(gr_labels, gl2);

    // --- Inline Rich Text Spans ---
    TspdfNode *rt_title = tspdf_layout_text(&ctx, "Inline Rich Text Spans", sans_bold, 16);
    rt_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, rt_title);

    // Rich text: mixed bold/italic/colors in one line
    TspdfNode *rt1 = tspdf_layout_text(&ctx, "", sans, 12);  // base (unused when spans present)
    rt1->width = tspdf_size_grow();
    tspdf_layout_text_add_span(rt1, "This is ", sans, 12, tspdf_color_from_u8(50, 50, 70), 0);
    tspdf_layout_text_add_span(rt1, "bold", sans_bold, 12, tspdf_color_from_u8(50, 50, 70), 0);
    tspdf_layout_text_add_span(rt1, " and colored", sans_bold, 12, tspdf_color_from_u8(231, 76, 60), TSPDF_TEXT_DECOR_UNDERLINE);
    tspdf_layout_text_add_span(rt1, " text in one line.", sans, 12, tspdf_color_from_u8(50, 50, 70), 0);
    tspdf_layout_add_child(p6_content, rt1);

    TspdfNode *rt2 = tspdf_layout_text(&ctx, "", mono, 10);
    rt2->width = tspdf_size_grow();
    tspdf_layout_text_add_span(rt2, "Code: ", sans, 10, tspdf_color_from_u8(100, 100, 120), 0);
    tspdf_layout_text_add_span(rt2, "tspdf_writer_create()", mono, 10, tspdf_color_from_u8(52, 152, 219), 0);
    tspdf_layout_text_add_span(rt2, " returns a new doc.", sans, 10, tspdf_color_from_u8(100, 100, 120), 0);
    tspdf_layout_add_child(p6_content, rt2);

    // --- Page Numbers Info ---
    TspdfNode *pn_title = tspdf_layout_text(&ctx, "Page Numbers", sans_bold, 16);
    pn_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p6_content, pn_title);

    TspdfNode *pn_desc = tspdf_layout_text(&ctx,
        "The tspdf_layout_render_page_number() function draws 'Page X of Y' at configurable "
        "position and alignment. See the paginated table pages for a live example with "
        "centered page numbers at the bottom.",
        sans, 11);
    pn_desc->text.color = tspdf_color_from_u8(80, 80, 100);
    pn_desc->width = tspdf_size_grow();
    pn_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p6_content, pn_desc);

    // Compute & render page 6
    tspdf_layout_compute(&ctx, root6, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root6, page6);

    // Draw multi-stop gradient manually
    {
        TspdfGradientStop stops[] = {
            {0.0, tspdf_color_from_u8(231, 76, 60)},    // red
            {0.25, tspdf_color_from_u8(241, 196, 15)},   // yellow
            {0.5, tspdf_color_from_u8(46, 204, 113)},    // green
            {0.75, tspdf_color_from_u8(52, 152, 219)},   // blue
            {1.0, tspdf_color_from_u8(155, 89, 182)},    // purple
        };
        double gx = grad_multi->computed_x;
        double gw = grad_multi->computed_width;
        double gy_layout = grad_multi->computed_y;
        double gh = grad_multi->computed_height;
        double gy_pdf = TSPDF_PAGE_A4_HEIGHT - gy_layout - gh;

        const char *multi_grad = tspdf_writer_add_gradient_stops(doc,
            gx, gy_pdf, gx + gw, gy_pdf, stops, 5);

        tspdf_stream_save(page6);
        tspdf_stream_rounded_rect_pdf(page6, gx, gy_pdf, gw, gh, 6, 6, 6, 6);
        tspdf_stream_clip(page6);
        tspdf_stream_fill_gradient(page6, multi_grad);
        tspdf_stream_restore(page6);
    }

    // Draw radial gradient manually
    {
        double rx = grad_radial_box->computed_x;
        double rw = grad_radial_box->computed_width;
        double ry_layout = grad_radial_box->computed_y;
        double rh = grad_radial_box->computed_height;
        double ry_pdf = TSPDF_PAGE_A4_HEIGHT - ry_layout - rh;
        double cx = rx + rw / 2;
        double cy = ry_pdf + rh / 2;
        double radius = rh / 2;

        const char *radial_grad = tspdf_writer_add_radial_gradient(doc,
            cx, cy, 0, radius,
            tspdf_color_from_u8(241, 196, 15),   // yellow center
            tspdf_color_from_u8(231, 76, 60));    // red edge

        tspdf_stream_save(page6);
        tspdf_stream_rounded_rect_pdf(page6, rx, ry_pdf, rw, rh, 6, 6, 6, 6);
        tspdf_stream_clip(page6);
        tspdf_stream_fill_gradient(page6, radial_grad);
        tspdf_stream_restore(page6);
    }

    int pg_shadows = pg_new_features + 1;
    int bm_page6 = tspdf_writer_add_bookmark(doc, "Shadows & Page Numbers", pg_shadows);
    (void)bm_page6;

    // ==========================================
    // PAGE 7: Colspan Tables & Form Fields
    // ==========================================
    tspdf_layout_tree_free(root6);
    tspdf_arena_reset(&layout_arena);

    TspdfStream *page7 = tspdf_writer_add_page(doc);

    TspdfNode *root7 = tspdf_layout_box(&ctx);
    root7->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root7->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root7->direction = TSPDF_DIR_COLUMN;
    root7->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root7);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    // Header
    TspdfNode *p7_header = tspdf_layout_box(&ctx);
    p7_header->width = tspdf_size_grow();
    p7_header->height = tspdf_size_fixed(60);
    p7_header->direction = TSPDF_DIR_ROW;
    p7_header->align_y = TSPDF_ALIGN_CENTER;
    p7_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p7_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root7, p7_header);

    TspdfNode *p7_title = tspdf_layout_text(&ctx, "Page 7: Colspan Tables & Form Fields", sans_bold, 20);
    p7_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p7_header, p7_title);

    TspdfNode *p7_content = tspdf_layout_box(&ctx);
    p7_content->width = tspdf_size_grow();
    p7_content->height = tspdf_size_grow();
    p7_content->direction = TSPDF_DIR_COLUMN;
    p7_content->padding = tspdf_padding_all(30);
    p7_content->gap = 18;
    tspdf_layout_add_child(root7, p7_content);

    // --- Colspan Table ---
    TspdfNode *cs_title = tspdf_layout_text(&ctx, "Table with Colspan", sans_bold, 16);
    cs_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p7_content, cs_title);

    TspdfNode *cs_desc = tspdf_layout_text(&ctx,
        "Cells can span multiple columns using tspdf_layout_table_add_row_spans().",
        sans, 10);
    cs_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    cs_desc->width = tspdf_size_grow();
    cs_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p7_content, cs_desc);

    {
        TspdfTableStyle cs_style = {
            .font_name = sans,
            .font_size = 10,
            .text_color = tspdf_color_from_u8(50, 50, 70),
            .header_bg = tspdf_color_from_u8(30, 30, 46),
            .header_text_color = tspdf_color_rgb(1, 1, 1),
            .row_bg_even = tspdf_color_rgb(1, 1, 1),
            .row_bg_odd = tspdf_color_from_u8(248, 248, 252),
            .border_color = tspdf_color_from_u8(220, 220, 230),
            .border_width = 0.5,
            .row_height = 30,
            .header_height = 35,
            .padding = 8,
        };

        const char *cs_headers[] = {"Item", "Q1", "Q2", "Q3", "Q4"};
        double cs_widths[] = {0.28, 0.18, 0.18, 0.18, 0.18};
        TspdfNode *cs_tbl = tspdf_layout_table(&ctx, cs_headers, cs_widths, 5, cs_style);
        tspdf_layout_add_child(p7_content, cs_tbl);

        // Normal row
        const char *row1[] = {"Revenue", "$120k", "$145k", "$132k", "$168k"};
        tspdf_layout_table_add_row(&ctx, cs_tbl, row1, cs_widths, 5, cs_style);

        // Normal row
        const char *row2[] = {"Expenses", "$95k", "$102k", "$98k", "$115k"};
        tspdf_layout_table_add_row(&ctx, cs_tbl, row2, cs_widths, 5, cs_style);

        // Colspan row: "Total Profit" spanning all Q columns
        const char *row3[] = {"Net Profit", "$133k (Annual Total)"};
        int spans3[] = {1, 4};  // first cell = 1 col, second cell = 4 cols
        tspdf_layout_table_add_row_spans(&ctx, cs_tbl, row3, spans3, 2, cs_widths, 5, cs_style);

        // Another colspan example: summary spanning 3 cols
        const char *row4[] = {"H1 Summary", "$267k", "H2 Summary", "$300k"};
        int spans4[] = {1, 2, 1, 1};
        tspdf_layout_table_add_row_spans(&ctx, cs_tbl, row4, spans4, 4, cs_widths, 5, cs_style);
    }

    // --- Form Fields (AcroForm) ---
    TspdfNode *form_title = tspdf_layout_text(&ctx, "Interactive Form Fields (AcroForm)", sans_bold, 16);
    form_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p7_content, form_title);

    TspdfNode *form_desc = tspdf_layout_text(&ctx,
        "PDF AcroForm support: text input fields and checkboxes. "
        "These are interactive when opened in a PDF viewer like Acrobat or Evince.",
        sans, 10);
    form_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    form_desc->width = tspdf_size_grow();
    form_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p7_content, form_desc);

    // Form layout — we need placeholder boxes to know where to place form fields
    TspdfNode *form_box = tspdf_layout_box(&ctx);
    form_box->width = tspdf_size_grow();
    form_box->height = tspdf_size_fit();
    form_box->direction = TSPDF_DIR_COLUMN;
    form_box->gap = 12;
    form_box->padding = tspdf_padding_all(20);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, form_box);
        s->has_background = true;
        s->background = tspdf_color_from_u8(250, 250, 255);
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(200, 200, 220);
        s->border_width = 1;
        s->corner_radius[0] = 8;
        s->corner_radius[1] = 8;
        s->corner_radius[2] = 8;
        s->corner_radius[3] = 8;
    }
    tspdf_layout_add_child(p7_content, form_box);

    // Row 1: First Name
    TspdfNode *fname_row = tspdf_layout_box(&ctx);
    fname_row->width = tspdf_size_grow();
    fname_row->height = tspdf_size_fixed(28);
    fname_row->direction = TSPDF_DIR_ROW;
    fname_row->gap = 10;
    fname_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(form_box, fname_row);

    TspdfNode *fname_lbl = tspdf_layout_text(&ctx, "First Name:", sans_bold, 11);
    fname_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    fname_lbl->width = tspdf_size_fixed(100);
    tspdf_layout_add_child(fname_row, fname_lbl);

    TspdfNode *fname_field = tspdf_layout_box(&ctx);
    fname_field->width = tspdf_size_grow();
    fname_field->height = tspdf_size_fixed(24);
    tspdf_layout_add_child(fname_row, fname_field);

    // Row 2: Last Name
    TspdfNode *lname_row = tspdf_layout_box(&ctx);
    lname_row->width = tspdf_size_grow();
    lname_row->height = tspdf_size_fixed(28);
    lname_row->direction = TSPDF_DIR_ROW;
    lname_row->gap = 10;
    lname_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(form_box, lname_row);

    TspdfNode *lname_lbl = tspdf_layout_text(&ctx, "Last Name:", sans_bold, 11);
    lname_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    lname_lbl->width = tspdf_size_fixed(100);
    tspdf_layout_add_child(lname_row, lname_lbl);

    TspdfNode *lname_field = tspdf_layout_box(&ctx);
    lname_field->width = tspdf_size_grow();
    lname_field->height = tspdf_size_fixed(24);
    tspdf_layout_add_child(lname_row, lname_field);

    // Row 3: Email
    TspdfNode *email_row = tspdf_layout_box(&ctx);
    email_row->width = tspdf_size_grow();
    email_row->height = tspdf_size_fixed(28);
    email_row->direction = TSPDF_DIR_ROW;
    email_row->gap = 10;
    email_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(form_box, email_row);

    TspdfNode *email_lbl = tspdf_layout_text(&ctx, "Email:", sans_bold, 11);
    email_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    email_lbl->width = tspdf_size_fixed(100);
    tspdf_layout_add_child(email_row, email_lbl);

    TspdfNode *email_field = tspdf_layout_box(&ctx);
    email_field->width = tspdf_size_grow();
    email_field->height = tspdf_size_fixed(24);
    tspdf_layout_add_child(email_row, email_field);

    // Row 4: Checkboxes
    TspdfNode *check_row = tspdf_layout_box(&ctx);
    check_row->width = tspdf_size_grow();
    check_row->height = tspdf_size_fixed(28);
    check_row->direction = TSPDF_DIR_ROW;
    check_row->gap = 10;
    check_row->align_y = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(form_box, check_row);

    TspdfNode *check_lbl = tspdf_layout_text(&ctx, "Options:", sans_bold, 11);
    check_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    check_lbl->width = tspdf_size_fixed(100);
    tspdf_layout_add_child(check_row, check_lbl);

    TspdfNode *cb1_box = tspdf_layout_box(&ctx);
    cb1_box->width = tspdf_size_fixed(14);
    cb1_box->height = tspdf_size_fixed(14);
    tspdf_layout_add_child(check_row, cb1_box);

    TspdfNode *cb1_lbl = tspdf_layout_text(&ctx, "Subscribe", sans, 10);
    cb1_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(check_row, cb1_lbl);

    TspdfNode *cb2_box = tspdf_layout_box(&ctx);
    cb2_box->width = tspdf_size_fixed(14);
    cb2_box->height = tspdf_size_fixed(14);
    tspdf_layout_add_child(check_row, cb2_box);

    TspdfNode *cb2_lbl = tspdf_layout_text(&ctx, "Agree to terms", sans, 10);
    cb2_lbl->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(check_row, cb2_lbl);

    // Compute & render page 7
    tspdf_layout_compute(&ctx, root7, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root7, page7);

    // Now add form fields at computed positions (convert layout coords to PDF coords)
    int pg_colspan = pg_shadows + 1;
    {
        double page_h = TSPDF_PAGE_A4_HEIGHT;
        // Text fields
        double fx = fname_field->computed_x;
        double fy = page_h - fname_field->computed_y - fname_field->computed_height;
        tspdf_writer_add_text_field(doc, pg_colspan, "first_name",
            fx, fy, fname_field->computed_width, fname_field->computed_height,
            "John", sans, 11);

        fx = lname_field->computed_x;
        fy = page_h - lname_field->computed_y - lname_field->computed_height;
        tspdf_writer_add_text_field(doc, pg_colspan, "last_name",
            fx, fy, lname_field->computed_width, lname_field->computed_height,
            "Doe", sans, 11);

        fx = email_field->computed_x;
        fy = page_h - email_field->computed_y - email_field->computed_height;
        tspdf_writer_add_text_field(doc, pg_colspan, "email",
            fx, fy, email_field->computed_width, email_field->computed_height,
            "john@example.com", sans, 11);

        // Checkboxes
        fx = cb1_box->computed_x;
        fy = page_h - cb1_box->computed_y - cb1_box->computed_height;
        tspdf_writer_add_checkbox(doc, pg_colspan, "subscribe",
            fx, fy, cb1_box->computed_height, true);

        fx = cb2_box->computed_x;
        fy = page_h - cb2_box->computed_y - cb2_box->computed_height;
        tspdf_writer_add_checkbox(doc, pg_colspan, "agree_terms",
            fx, fy, cb2_box->computed_height, false);
    }

    int bm_page7 = tspdf_writer_add_bookmark(doc, "Colspan & Forms", pg_colspan);
    tspdf_writer_add_child_bookmark(doc, bm_page7, "Colspan Table", pg_colspan);
    tspdf_writer_add_child_bookmark(doc, bm_page7, "Form Fields", pg_colspan);

    // ==========================================
    // PAGE 8: Vector Paths
    // ==========================================
    tspdf_layout_tree_free(root7);
    tspdf_arena_reset(&layout_arena);

    TspdfStream *page8 = tspdf_writer_add_page(doc);

    TspdfNode *root8 = tspdf_layout_box(&ctx);
    root8->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root8->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root8->direction = TSPDF_DIR_COLUMN;
    root8->padding = tspdf_padding_all(0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root8);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    // Header
    TspdfNode *p8_header = tspdf_layout_box(&ctx);
    p8_header->width = tspdf_size_grow();
    p8_header->height = tspdf_size_fixed(60);
    p8_header->direction = TSPDF_DIR_ROW;
    p8_header->align_y = TSPDF_ALIGN_CENTER;
    p8_header->padding = tspdf_padding_xy(30, 0);
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, p8_header);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    tspdf_layout_add_child(root8, p8_header);

    TspdfNode *p8_title = tspdf_layout_text(&ctx, "Page 8: Vector Paths & Custom Drawing", sans_bold, 20);
    p8_title->text.color = tspdf_color_rgb(1, 1, 1);
    tspdf_layout_add_child(p8_header, p8_title);

    TspdfNode *p8_content = tspdf_layout_box(&ctx);
    p8_content->width = tspdf_size_grow();
    p8_content->height = tspdf_size_grow();
    p8_content->direction = TSPDF_DIR_COLUMN;
    p8_content->padding = tspdf_padding_all(30);
    p8_content->gap = 15;
    tspdf_layout_add_child(root8, p8_content);

    // Description
    TspdfNode *vp_desc = tspdf_layout_text(&ctx,
        "Custom vector paths using move_to, line_to, curve_to, arc, and close. "
        "Coordinates are relative to the node's bounds (y-down).",
        sans, 10);
    vp_desc->text.color = tspdf_color_from_u8(100, 100, 120);
    vp_desc->width = tspdf_size_grow();
    vp_desc->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(p8_content, vp_desc);

    TspdfNode *shapes_row = tspdf_layout_box(&ctx);
    shapes_row->width = tspdf_size_grow();
    shapes_row->height = tspdf_size_fixed(130);
    shapes_row->direction = TSPDF_DIR_ROW;
    shapes_row->gap = 20;
    shapes_row->align_x = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p8_content, shapes_row);

    // 1. Triangle
    {
        TspdfNode *tri = tspdf_layout_box(&ctx);
        tri->width = tspdf_size_fixed(100);
        tri->height = tspdf_size_fixed(100);
        tspdf_layout_add_child(shapes_row, tri);

        TspdfPathConfig *p = tspdf_layout_path_begin(&ctx, tri);
        tspdf_layout_path_move_to(p, 50, 10);   // top center
        tspdf_layout_path_line_to(p, 90, 90);   // bottom right
        tspdf_layout_path_line_to(p, 10, 90);   // bottom left
        tspdf_layout_path_close(p);
        tspdf_layout_path_set_fill(p, tspdf_color_from_u8(52, 152, 219));
        tspdf_layout_path_set_stroke(p, tspdf_color_from_u8(30, 30, 46), 2);
    }

    // 2. Star (5-pointed)
    {
        TspdfNode *star = tspdf_layout_box(&ctx);
        star->width = tspdf_size_fixed(100);
        star->height = tspdf_size_fixed(100);
        tspdf_layout_add_child(shapes_row, star);

        TspdfPathConfig *p = tspdf_layout_path_begin(&ctx, star);
        double cx = 50, cy = 50, r_outer = 40, r_inner = 16;
        for (int i = 0; i < 5; i++) {
            double a_outer = (-90 + i * 72) * M_PI / 180.0;
            double a_inner = (-90 + i * 72 + 36) * M_PI / 180.0;
            double ox = cx + r_outer * cos(a_outer);
            double oy = cy + r_outer * sin(a_outer);
            double ix = cx + r_inner * cos(a_inner);
            double iy = cy + r_inner * sin(a_inner);
            if (i == 0) tspdf_layout_path_move_to(p, ox, oy);
            else tspdf_layout_path_line_to(p, ox, oy);
            tspdf_layout_path_line_to(p, ix, iy);
        }
        tspdf_layout_path_close(p);
        tspdf_layout_path_set_fill(p, tspdf_color_from_u8(241, 196, 15));
        tspdf_layout_path_set_stroke(p, tspdf_color_from_u8(230, 126, 34), 1.5);
    }

    // 3. Heart (Bezier curves)
    {
        TspdfNode *heart = tspdf_layout_box(&ctx);
        heart->width = tspdf_size_fixed(100);
        heart->height = tspdf_size_fixed(100);
        tspdf_layout_add_child(shapes_row, heart);

        TspdfPathConfig *p = tspdf_layout_path_begin(&ctx, heart);
        // Heart shape using cubic Bezier curves
        tspdf_layout_path_move_to(p, 50, 85);  // bottom point
        tspdf_layout_path_curve_to(p, 50, 75, 20, 55, 15, 45);  // left lobe
        tspdf_layout_path_curve_to(p, 8, 30, 15, 10, 35, 10);
        tspdf_layout_path_curve_to(p, 45, 10, 50, 18, 50, 25);  // top left to center
        tspdf_layout_path_curve_to(p, 50, 18, 55, 10, 65, 10);  // center to top right
        tspdf_layout_path_curve_to(p, 85, 10, 92, 30, 85, 45);
        tspdf_layout_path_curve_to(p, 80, 55, 50, 75, 50, 85);  // right lobe back to bottom
        tspdf_layout_path_close(p);
        tspdf_layout_path_set_fill(p, tspdf_color_from_u8(231, 76, 60));
    }

    // 4. Arc / pie slice
    {
        TspdfNode *pie = tspdf_layout_box(&ctx);
        pie->width = tspdf_size_fixed(100);
        pie->height = tspdf_size_fixed(100);
        tspdf_layout_add_child(shapes_row, pie);

        TspdfPathConfig *p = tspdf_layout_path_begin(&ctx, pie);
        tspdf_layout_path_move_to(p, 50, 50);  // center
        tspdf_layout_path_arc(p, 50, 50, 40, -90, 270);  // 270-degree arc
        tspdf_layout_path_close(p);
        tspdf_layout_path_set_fill(p, tspdf_color_from_u8(46, 204, 113));
        tspdf_layout_path_set_stroke(p, tspdf_color_from_u8(30, 30, 46), 1.5);
    }

    // Labels row
    TspdfNode *labels_row = tspdf_layout_box(&ctx);
    labels_row->width = tspdf_size_grow();
    labels_row->height = tspdf_size_fit();
    labels_row->direction = TSPDF_DIR_ROW;
    labels_row->gap = 20;
    labels_row->align_x = TSPDF_ALIGN_CENTER;
    tspdf_layout_add_child(p8_content, labels_row);

    const char *shape_names[] = {"Triangle", "Star", "Heart", "Pie Chart"};
    for (int i = 0; i < 4; i++) {
        TspdfNode *lbl = tspdf_layout_text(&ctx, shape_names[i], sans, 10);
        lbl->text.color = tspdf_color_from_u8(80, 80, 100);
        lbl->width = tspdf_size_fixed(100);
        lbl->text.alignment = TSPDF_TEXT_ALIGN_CENTER;
        tspdf_layout_add_child(labels_row, lbl);
    }

    // Line chart demo
    TspdfNode *chart_title = tspdf_layout_text(&ctx, "Line Chart (Vector Paths)", sans_bold, 16);
    chart_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p8_content, chart_title);

    {
        TspdfNode *chart = tspdf_layout_box(&ctx);
        chart->width = tspdf_size_grow();
        chart->height = tspdf_size_fixed(200);
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, chart);
            s->has_background = true;
            s->background = tspdf_color_from_u8(248, 248, 255);
            s->has_border = true;
            s->border_color = tspdf_color_from_u8(200, 200, 220);
            s->border_width = 1;
            s->corner_radius[0] = 6;
            s->corner_radius[1] = 6;
            s->corner_radius[2] = 6;
            s->corner_radius[3] = 6;
        }
        tspdf_layout_add_child(p8_content, chart);

        // Chart axes
        TspdfPathConfig *axes = tspdf_layout_path_begin(&ctx, chart);
        tspdf_layout_path_move_to(axes, 40, 10);    // y-axis top
        tspdf_layout_path_line_to(axes, 40, 180);   // y-axis bottom
        tspdf_layout_path_line_to(axes, 510, 180);  // x-axis right
        tspdf_layout_path_set_stroke(axes, tspdf_color_from_u8(100, 100, 120), 1.5);

        // We'll draw the data lines manually after layout since we need
        // the chart to be a separate path. Instead, add data line as child path.
    }

    // Data line overlay
    {
        TspdfNode *chart_ref = p8_content->children[p8_content->child_count - 1];

        // First data series — fills entire chart area
        TspdfNode *data_line = tspdf_layout_box(&ctx);
        data_line->width = tspdf_size_grow();
        data_line->height = tspdf_size_grow();
        tspdf_layout_add_child(chart_ref, data_line);

        TspdfPathConfig *line = tspdf_layout_path_begin(&ctx, data_line);
        double data[] = {120, 80, 140, 60, 110, 50, 90, 70, 40, 30, 55, 35};
        int n = 12;
        for (int i = 0; i < n; i++) {
            double x = 50 + i * 40;
            double y = 20 + data[i];
            if (i == 0) tspdf_layout_path_move_to(line, x, y);
            else tspdf_layout_path_line_to(line, x, y);
        }
        tspdf_layout_path_set_stroke(line, tspdf_color_from_u8(52, 152, 219), 2.5);

        // Second data series — nested inside first so both share same coordinate space
        TspdfNode *data_line2 = tspdf_layout_box(&ctx);
        data_line2->width = tspdf_size_grow();
        data_line2->height = tspdf_size_grow();
        tspdf_layout_add_child(data_line, data_line2);

        TspdfPathConfig *line2 = tspdf_layout_path_begin(&ctx, data_line2);
        double data2[] = {60, 70, 55, 95, 130, 100, 85, 110, 90, 120, 140, 155};
        for (int i = 0; i < n; i++) {
            double x = 50 + i * 40;
            double y = 20 + data2[i];
            if (i == 0) tspdf_layout_path_move_to(line2, x, y);
            else tspdf_layout_path_line_to(line2, x, y);
        }
        tspdf_layout_path_set_stroke(line2, tspdf_color_from_u8(231, 76, 60), 2.5);
    }

    // Font subsetting info
    TspdfNode *subset_title = tspdf_layout_text(&ctx, "Font Subsetting", sans_bold, 16);
    subset_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(p8_content, subset_title);

    TspdfNode *subset_desc = tspdf_layout_text(&ctx,
        "TrueType fonts are now automatically subset during PDF generation. "
        "Only glyphs for characters actually used in the document are embedded, "
        "reducing font data by 60-80%. Composite glyphs (accented characters) "
        "automatically include their component glyphs.",
        sans, 11);
    subset_desc->text.color = tspdf_color_from_u8(80, 80, 100);
    subset_desc->width = tspdf_size_grow();
    subset_desc->text.wrap = TSPDF_WRAP_WORD;
    subset_desc->text.line_height_factor = 1.5;
    tspdf_layout_add_child(p8_content, subset_desc);

    // Compute & render page 8
    tspdf_layout_compute(&ctx, root8, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root8, page8);

    int pg_vectors = pg_colspan + 1;
    int bm_page8 = tspdf_writer_add_bookmark(doc, "Vector Paths", pg_vectors);
    tspdf_writer_add_child_bookmark(doc, bm_page8, "Shapes", pg_vectors);
    tspdf_writer_add_child_bookmark(doc, bm_page8, "Line Chart", pg_vectors);

    // ==========================================
    // PAGES 9+: Auto Page Break with Repeating Headers
    // ==========================================
    tspdf_layout_tree_free(root8);
    tspdf_arena_reset(&layout_arena);

    TspdfNode *table_root = tspdf_layout_box(&ctx);
    table_root->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    table_root->height = tspdf_size_fit();  // unlimited — will be paginated
    table_root->direction = TSPDF_DIR_COLUMN;
    table_root->padding = tspdf_padding_all(30);
    table_root->gap = 0;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, table_root);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
    }

    // Table header row — marked as repeating
    TspdfNode *header_row = tspdf_layout_box(&ctx);
    header_row->width = tspdf_size_grow();
    header_row->height = tspdf_size_fixed(40);
    header_row->direction = TSPDF_DIR_ROW;
    header_row->align_y = TSPDF_ALIGN_CENTER;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, header_row);
        s->has_background = true;
        s->background = tspdf_color_from_u8(30, 30, 46);
    }
    header_row->repeat_mode = TSPDF_REPEAT_ALL;  // <-- THIS IS THE KEY
    tspdf_layout_add_child(table_root, header_row);

    const char *col_headers[] = {"#", "Name", "Category", "Status", "Score"};
    double col_widths[] = {0.08, 0.27, 0.25, 0.20, 0.20};
    for (int i = 0; i < 5; i++) {
        TspdfNode *cell = tspdf_layout_box(&ctx);
        cell->width = tspdf_size_percent(col_widths[i]);
        cell->height = tspdf_size_grow();
        cell->align_x = TSPDF_ALIGN_CENTER;
        cell->align_y = TSPDF_ALIGN_CENTER;
        tspdf_layout_add_child(header_row, cell);

        TspdfNode *ht = tspdf_layout_text(&ctx, col_headers[i], sans_bold, 11);
        ht->text.color = tspdf_color_rgb(1, 1, 1);
        tspdf_layout_add_child(cell, ht);
    }

    // Generate 40 data rows
    const char *names[] = {"Alice", "Bob", "Charlie", "Diana", "Eve",
                           "Frank", "Grace", "Hank", "Ivy", "Jack"};
    const char *categories[] = {"Engineering", "Design", "Marketing",
                                "Sales", "Support"};
    const char *statuses[] = {"Active", "Pending", "Completed", "Review"};

    for (int row = 0; row < 30; row++) {
        TspdfNode *data_row = tspdf_layout_box(&ctx);
        data_row->width = tspdf_size_grow();
        data_row->height = tspdf_size_fixed(32);
        data_row->direction = TSPDF_DIR_ROW;
        data_row->align_y = TSPDF_ALIGN_CENTER;
        {
            TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, data_row);
            s->has_background = true;
            s->background = (row % 2 == 0)
                ? tspdf_color_from_u8(248, 248, 252)
                : tspdf_color_rgb(1, 1, 1);
            s->has_border = true;
            s->border_color = tspdf_color_from_u8(230, 230, 240);
            s->border_width = 0.5;
        }
        tspdf_layout_add_child(table_root, data_row);

        // Cell data
        char num_str[8];
        snprintf(num_str, sizeof(num_str), "%d", row + 1);
        char score_str[8];
        snprintf(score_str, sizeof(score_str), "%.1f", 60.0 + (row * 7 % 40));

        const char *cell_data[] = {
            num_str,
            names[row % 10],
            categories[row % 5],
            statuses[row % 4],
            score_str
        };

        for (int c = 0; c < 5; c++) {
            TspdfNode *cell = tspdf_layout_box(&ctx);
            cell->width = tspdf_size_percent(col_widths[c]);
            cell->height = tspdf_size_grow();
            cell->align_x = (c == 0 || c == 4) ? TSPDF_ALIGN_CENTER : TSPDF_ALIGN_START;
            cell->align_y = TSPDF_ALIGN_CENTER;
            cell->padding = tspdf_padding_xy(8, 0);
            tspdf_layout_add_child(data_row, cell);

            TspdfNode *ct = tspdf_layout_text(&ctx, cell_data[c], sans, 10);
            ct->text.color = tspdf_color_from_u8(50, 50, 70);
            if (c == 3) {
                // Color-code status
                if (row % 4 == 0) ct->text.color = tspdf_color_from_u8(46, 204, 113);
                else if (row % 4 == 2) ct->text.color = tspdf_color_from_u8(52, 152, 219);
                else if (row % 4 == 1) ct->text.color = tspdf_color_from_u8(241, 196, 15);
                else ct->text.color = tspdf_color_from_u8(155, 89, 182);
            }
            tspdf_layout_add_child(cell, ct);
        }
    }

    // Paginate and render
    TspdfPaginationResult pagination;
    int num_pages = tspdf_layout_compute_paginated(&ctx, table_root, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT, &pagination);
    printf("Table paginated into %d pages\n", num_pages);

    for (int p = 0; p < num_pages; p++) {
        TspdfStream *table_page = tspdf_writer_add_page(doc);
        tspdf_layout_render_page(&ctx, table_root, &pagination, p, table_page);
        // Add page numbers at bottom center
        tspdf_layout_render_page_number(&ctx, table_page, p, num_pages,
                                   sans, 9, tspdf_color_from_u8(130, 130, 150),
                                   TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT,
                                   TSPDF_TEXT_ALIGN_CENTER, 20);
    }

    // ==========================================
    // PAGE 9 (before table): Unicode & Error Reporting Showcase
    // ==========================================
    tspdf_layout_tree_free(table_root);
    tspdf_arena_reset(&layout_arena);
    int pg_unicode = pg_vectors + 1;

    TspdfNode *root_uni = tspdf_layout_box(&ctx);
    root_uni->width = tspdf_size_fixed(TSPDF_PAGE_A4_WIDTH);
    root_uni->height = tspdf_size_fixed(TSPDF_PAGE_A4_HEIGHT);
    root_uni->direction = TSPDF_DIR_COLUMN;
    root_uni->padding = tspdf_padding_all(30);
    root_uni->gap = 15;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, root_uni);
        s->has_background = true;
        s->background = tspdf_color_from_u8(245, 245, 250);
    }

    // --- Title ---
    TspdfNode *uni_title = tspdf_layout_text(&ctx, "Unicode Support & Error Reporting", sans_bold, 24);
    uni_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(root_uni, uni_title);

    TspdfNode *uni_subtitle = tspdf_layout_text(&ctx,
        "Full Unicode via CIDFont Type2 + Identity-H encoding. "
        "Text is encoded as hex glyph IDs, with a ToUnicode CMap for copy/paste.",
        sans, 10);
    uni_subtitle->text.color = tspdf_color_from_u8(100, 100, 120);
    tspdf_layout_add_child(root_uni, uni_subtitle);

    // --- European Languages ---
    TspdfNode *euro_box = tspdf_layout_box(&ctx);
    euro_box->width = tspdf_size_grow();
    euro_box->height = tspdf_size_fit();
    euro_box->direction = TSPDF_DIR_COLUMN;
    euro_box->padding = tspdf_padding_all(20);
    euro_box->gap = 8;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, euro_box);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
        s->corner_radius[0] = 8; s->corner_radius[1] = 8;
        s->corner_radius[2] = 8; s->corner_radius[3] = 8;
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(200, 200, 220);
        s->border_width = 1;
    }
    tspdf_layout_add_child(root_uni, euro_box);

    TspdfNode *euro_title = tspdf_layout_text(&ctx, "European Languages", sans_bold, 14);
    euro_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(euro_box, euro_title);

    TspdfNode *french = tspdf_layout_text(&ctx,
        "Fran\xC3\xA7" "ais: Les \xC3\xA9l\xC3\xA8ves \xC3\xA9tudient la g\xC3\xA9om\xC3\xA9trie \xC3\xA0 l\xE2\x80\x99\xC3\xA9" "cole.",
        sans, 12);
    french->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(euro_box, french);

    TspdfNode *german = tspdf_layout_text(&ctx,
        "Deutsch: \xC3\x9C" "ber den Fl\xC3\xBC" "ssen liegt ein sch\xC3\xB6ner Gru\xC3\x9F.",
        sans, 12);
    german->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(euro_box, german);

    TspdfNode *spanish = tspdf_layout_text(&ctx,
        "Espa\xC3\xB1ol: \xC2\xBF" "Cu\xC3\xA1ntos a\xC3\xB1os tienes? \xC2\xA1" "Buenas noches!",
        sans, 12);
    spanish->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(euro_box, spanish);

    TspdfNode *portuguese = tspdf_layout_text(&ctx,
        "Portugu\xC3\xAAs: A crian\xC3\xA7" "a \xC3\xA9 muito simp\xC3\xA1tica e cora\xC3\xA7\xC3\xA3o.",
        sans, 12);
    portuguese->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(euro_box, portuguese);

    TspdfNode *swedish = tspdf_layout_text(&ctx,
        "Svenska: R\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s med \xC3\xA4gg och \xC3\xB6l.",
        sans, 12);
    swedish->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(euro_box, swedish);

    // --- Special Characters ---
    TspdfNode *special_box = tspdf_layout_box(&ctx);
    special_box->width = tspdf_size_grow();
    special_box->height = tspdf_size_fit();
    special_box->direction = TSPDF_DIR_COLUMN;
    special_box->padding = tspdf_padding_all(20);
    special_box->gap = 8;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, special_box);
        s->has_background = true;
        s->background = tspdf_color_rgb(1, 1, 1);
        s->corner_radius[0] = 8; s->corner_radius[1] = 8;
        s->corner_radius[2] = 8; s->corner_radius[3] = 8;
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(200, 200, 220);
        s->border_width = 1;
    }
    tspdf_layout_add_child(root_uni, special_box);

    TspdfNode *special_title = tspdf_layout_text(&ctx, "Symbols & Currency", sans_bold, 14);
    special_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(special_box, special_title);

    TspdfNode *currency = tspdf_layout_text(&ctx,
        "Currency: \xE2\x82\xAC" "100 \xC2\xA3" "50 \xC2\xA5" "1000 \xC2\xA2" "99",
        sans, 12);
    currency->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(special_box, currency);

    TspdfNode *symbols = tspdf_layout_text(&ctx,
        "Symbols: \xC2\xA9 2026 \xC2\xAE \xE2\x84\xA2 \xC2\xB1" "5\xC2\xB0 \xC3\x97 \xC3\xB7 \xE2\x89\xA0 \xE2\x89\xA4 \xE2\x89\xA5 \xE2\x88\x9E",
        sans, 12);
    symbols->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(special_box, symbols);

    TspdfNode *arrows = tspdf_layout_text(&ctx,
        "Arrows: \xE2\x86\x90 \xE2\x86\x91 \xE2\x86\x92 \xE2\x86\x93 \xE2\x86\x94 \xE2\x86\x95  Bullets: \xE2\x80\xA2 \xE2\x97\x8F \xE2\x97\x8B \xE2\x96\xB6 \xE2\x96\xA0",
        sans, 12);
    arrows->text.color = tspdf_color_from_u8(40, 40, 60);
    tspdf_layout_add_child(special_box, arrows);

    // --- Error Reporting ---
    TspdfNode *err_box = tspdf_layout_box(&ctx);
    err_box->width = tspdf_size_grow();
    err_box->height = tspdf_size_fit();
    err_box->direction = TSPDF_DIR_COLUMN;
    err_box->padding = tspdf_padding_all(20);
    err_box->gap = 8;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, err_box);
        s->has_background = true;
        s->background = tspdf_color_from_u8(255, 248, 240);
        s->corner_radius[0] = 8; s->corner_radius[1] = 8;
        s->corner_radius[2] = 8; s->corner_radius[3] = 8;
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(220, 180, 140);
        s->border_width = 1;
    }
    tspdf_layout_add_child(root_uni, err_box);

    TspdfNode *err_title = tspdf_layout_text(&ctx, "Error Reporting API", sans_bold, 14);
    err_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(err_box, err_title);

    // Demonstrate error reporting by triggering one and showing the result
    {
        TspdfWriter *tmp_doc = tspdf_writer_create();
        // Trigger an error: try to load a non-existent font
        const char *bad = tspdf_writer_add_ttf_font(tmp_doc, "/nonexistent/font.ttf");
        TspdfError e = tspdf_writer_last_error(tmp_doc);

        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
            "tspdf_writer_add_ttf_font(\"/nonexistent/font.ttf\") returned %s",
            bad ? "non-NULL (unexpected)" : "NULL (correct)");
        TspdfNode *err_line1 = tspdf_layout_text(&ctx, err_msg, mono, 10);
        err_line1->text.color = tspdf_color_from_u8(60, 60, 80);
        tspdf_layout_add_child(err_box, err_line1);

        snprintf(err_msg, sizeof(err_msg),
            "tspdf_writer_last_error() == %s (\"%s\")",
            e == TSPDF_ERR_IO ? "TSPDF_ERR_IO" :
            e == TSPDF_ERR_FONT_PARSE ? "TSPDF_ERR_FONT_PARSE" : "other",
            tspdf_error_string(e));
        TspdfNode *err_line2 = tspdf_layout_text(&ctx, err_msg, mono, 10);
        err_line2->text.color = tspdf_color_from_u8(180, 80, 40);
        tspdf_layout_add_child(err_box, err_line2);

        TspdfNode *err_note = tspdf_layout_text(&ctx,
            "No more fprintf(stderr) \xE2\x80\x94 all errors are programmatic via TspdfError enum.",
            sans, 11);
        err_note->text.color = tspdf_color_from_u8(100, 100, 120);
        tspdf_layout_add_child(err_box, err_note);

        tspdf_writer_destroy(tmp_doc);
    }

    // --- Implementation Note ---
    TspdfNode *impl_box = tspdf_layout_box(&ctx);
    impl_box->width = tspdf_size_grow();
    impl_box->height = tspdf_size_fit();
    impl_box->direction = TSPDF_DIR_COLUMN;
    impl_box->padding = tspdf_padding_all(20);
    impl_box->gap = 6;
    {
        TspdfBoxStyle *s = tspdf_layout_node_style(&ctx, impl_box);
        s->has_background = true;
        s->background = tspdf_color_from_u8(240, 245, 255);
        s->corner_radius[0] = 8; s->corner_radius[1] = 8;
        s->corner_radius[2] = 8; s->corner_radius[3] = 8;
        s->has_border = true;
        s->border_color = tspdf_color_from_u8(180, 200, 230);
        s->border_width = 1;
    }
    tspdf_layout_add_child(root_uni, impl_box);

    TspdfNode *impl_title = tspdf_layout_text(&ctx, "How It Works", sans_bold, 14);
    impl_title->text.color = tspdf_color_from_u8(30, 30, 46);
    tspdf_layout_add_child(impl_box, impl_title);

    TspdfNode *impl1 = tspdf_layout_text(&ctx,
        "1. Text input is UTF-8. The layout engine wraps at codepoint boundaries.",
        sans, 11);
    impl1->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(impl_box, impl1);

    TspdfNode *impl2 = tspdf_layout_text(&ctx,
        "2. At render time, UTF-8 is decoded to codepoints, mapped to glyph IDs via the font\xE2\x80\x99s cmap table.",
        sans, 11);
    impl2->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(impl_box, impl2);

    TspdfNode *impl3 = tspdf_layout_text(&ctx,
        "3. Glyph IDs are written as hex strings: <004800650066> Tj",
        mono, 10);
    impl3->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(impl_box, impl3);

    TspdfNode *impl4 = tspdf_layout_text(&ctx,
        "4. Font is embedded as CIDFont Type2 with Identity-H encoding and a ToUnicode CMap.",
        sans, 11);
    impl4->text.color = tspdf_color_from_u8(50, 50, 70);
    tspdf_layout_add_child(impl_box, impl4);

    TspdfStream *page_uni = tspdf_writer_add_page(doc);
    tspdf_layout_compute(&ctx, root_uni, TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT);
    tspdf_layout_render(&ctx, root_uni, page_uni);

    int bm_unicode = tspdf_writer_add_bookmark(doc, "Unicode & Errors", pg_unicode);
    tspdf_writer_add_child_bookmark(doc, bm_unicode, "European Languages", pg_unicode);
    tspdf_writer_add_child_bookmark(doc, bm_unicode, "Symbols & Currency", pg_unicode);
    tspdf_writer_add_child_bookmark(doc, bm_unicode, "Error Reporting", pg_unicode);
    tspdf_writer_add_child_bookmark(doc, bm_unicode, "How It Works", pg_unicode);

    // Add bookmarks for table pages
    int pg_table = pg_unicode + 1;
    int bm_table = tspdf_writer_add_bookmark(doc, "Data Table", pg_table);
    for (int p = 0; p < num_pages; p++) {
        char bm_title[64];
        snprintf(bm_title, sizeof(bm_title), "Table Page %d", p + 1);
        tspdf_writer_add_child_bookmark(doc, bm_table, bm_title, pg_table + p);
    }

    // Save
    const char *output_path = "output.pdf";
    if (tspdf_writer_save(doc, output_path) == TSPDF_OK) {
        printf("PDF written to %s\n", output_path);
    } else {
        printf("Failed to write PDF\n");
        tspdf_writer_destroy(doc);
        tspdf_layout_tree_free(root_uni);
        tspdf_arena_destroy(&layout_arena);
        return 1;
    }

    tspdf_writer_destroy(doc);
    tspdf_layout_tree_free(root_uni);
    tspdf_arena_destroy(&layout_arena);
    return 0;
}
