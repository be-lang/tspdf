#define _USE_MATH_DEFINES
#include "layout.h"
#include "../pdf/tspdf_writer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Free realloc'd arrays ---

void tspdf_layout_tree_free(TspdfNode *root) {
    if (!root) return;
    // Free children's subtrees first (children may be NULL for leaf nodes)
    if (root->children) {
        for (int i = 0; i < root->child_count; i++) {
            tspdf_layout_tree_free(root->children[i]);
        }
        free(root->children);
        root->children = NULL;
    }
    free(root->text.spans);
    root->text.spans = NULL;
    free(root->computed_text.lines);
    root->computed_text.lines = NULL;
    free(root->computed_text.line_widths);
    root->computed_text.line_widths = NULL;
}

// --- Context creation ---

TspdfLayout tspdf_layout_create(TspdfArena *arena) {
    TspdfLayout ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena = arena;
    return ctx;
}

// --- Helpers ---

static const char *arena_strdup(TspdfArena *arena, const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)tspdf_arena_alloc(arena, len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

// Lazily allocate TspdfBoxStyle on first use
static TspdfBoxStyle *ensure_style(TspdfLayout *ctx, TspdfNode *node) {
    if (!node->style) {
        node->style = (TspdfBoxStyle *)tspdf_arena_alloc_zero(ctx->arena, sizeof(TspdfBoxStyle));
    }
    return node->style;
}

TspdfBoxStyle *tspdf_layout_node_style(TspdfLayout *ctx, TspdfNode *node) {
    return ensure_style(ctx, node);
}

// --- Node creation ---

TspdfNode *tspdf_layout_box(TspdfLayout *ctx) {
    TspdfNode *node = (TspdfNode *)tspdf_arena_alloc_zero(ctx->arena, sizeof(TspdfNode));
    if (!node) return NULL;
    node->type = TSPDF_NODE_BOX;
    node->width = tspdf_size_fit();
    node->height = tspdf_size_fit();
    node->direction = TSPDF_DIR_COLUMN;
    node->align_x = TSPDF_ALIGN_START;
    node->align_y = TSPDF_ALIGN_START;
    return node;
}

TspdfNode *tspdf_layout_text(TspdfLayout *ctx, const char *text, const char *font_name, double font_size) {
    TspdfNode *node = (TspdfNode *)tspdf_arena_alloc_zero(ctx->arena, sizeof(TspdfNode));
    if (!node) return NULL;
    node->type = TSPDF_NODE_TEXT;
    node->width = tspdf_size_fit();
    node->height = tspdf_size_fit();
    node->text.text = arena_strdup(ctx->arena, text);
    node->text.font_name = arena_strdup(ctx->arena, font_name);
    node->text.font_size = font_size;
    node->text.color = tspdf_color_rgb(0, 0, 0);
    node->text.alignment = TSPDF_TEXT_ALIGN_LEFT;
    node->text.wrap = TSPDF_WRAP_WORD;
    node->text.line_height_factor = 1.2;
    return node;
}

TspdfError tspdf_layout_add_child(TspdfNode *parent, TspdfNode *child) {
    if (!parent || !child) return TSPDF_ERR_INVALID_ARG;
    if (parent->child_count >= parent->child_capacity) {
        int new_cap = parent->child_capacity == 0 ? 8 : parent->child_capacity * 2;
        if (new_cap > TSPDF_LAYOUT_MAX_CHILDREN) new_cap = TSPDF_LAYOUT_MAX_CHILDREN;
        if (parent->child_count >= new_cap) return TSPDF_ERR_OVERFLOW;
        TspdfNode **new_children = (TspdfNode **)realloc(parent->children,
            (size_t)new_cap * sizeof(TspdfNode *));
        if (!new_children) return TSPDF_ERR_ALLOC;
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return TSPDF_OK;
}

bool tspdf_layout_text_add_span(TspdfNode *node, const char *text,
                           const char *font_name, double font_size,
                           TspdfColor color, int decoration) {
    if (node->text.span_count >= node->text.span_capacity) {
        int new_cap = node->text.span_capacity == 0 ? 2 : node->text.span_capacity * 2;
        if (new_cap > TSPDF_LAYOUT_MAX_SPANS) new_cap = TSPDF_LAYOUT_MAX_SPANS;
        if (node->text.span_count >= new_cap) return false;
        TspdfTextSpan *new_spans = (TspdfTextSpan *)realloc(node->text.spans,
            (size_t)new_cap * sizeof(TspdfTextSpan));
        if (!new_spans) return false;
        node->text.spans = new_spans;
        node->text.span_capacity = new_cap;
    }
    TspdfTextSpan *span = &node->text.spans[node->text.span_count++];
    memset(span, 0, sizeof(*span));
    span->text = text;
    span->font_name = font_name;
    span->font_size = font_size;
    span->color = color;
    span->decoration = decoration;
    return true;
}

// --- Text helpers ---

static double measure_text(TspdfLayout *ctx, const char *font_name, double font_size, const char *text) {
    if (ctx->measure_text) {
        return ctx->measure_text(font_name, font_size, text, ctx->measure_userdata);
    }
    return strlen(text) * font_size * TSPDF_LAYOUT_FALLBACK_CHAR_WIDTH_FACTOR;
}

static double get_line_height(TspdfLayout *ctx, const char *font_name, double font_size) {
    if (ctx->font_line_height) {
        return ctx->font_line_height(font_name, font_size, ctx->line_height_userdata);
    }
    return font_size * TSPDF_LAYOUT_DEFAULT_LINE_HEIGHT_FACTOR;
}

// Emit a completed line into the node's computed_text
static void emit_line(TspdfLayout *ctx, TspdfNode *node,
                      const char *start, size_t len) {
    // Grow lines/widths arrays if needed
    if (node->computed_text.line_count >= node->computed_text.line_capacity) {
        int new_cap = node->computed_text.line_capacity == 0 ? 4 : node->computed_text.line_capacity * 2;
        if (new_cap > TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES) new_cap = TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES;
        if (node->computed_text.line_count >= new_cap) return;
        char **new_lines = (char **)realloc(node->computed_text.lines,
            (size_t)new_cap * sizeof(char *));
        double *new_widths = (double *)realloc(node->computed_text.line_widths,
            (size_t)new_cap * sizeof(double));
        if (!new_lines || !new_widths) { free(new_lines); free(new_widths); return; }
        node->computed_text.lines = new_lines;
        node->computed_text.line_widths = new_widths;
        node->computed_text.line_capacity = new_cap;
    }
    char *line = (char *)tspdf_arena_alloc(ctx->arena, len + 1);
    if (!line) return;
    memcpy(line, start, len);
    line[len] = '\0';
    int idx = node->computed_text.line_count;
    node->computed_text.lines[idx] = line;
    node->computed_text.line_widths[idx] = measure_text(
        ctx, node->text.font_name, node->text.font_size, line);
    node->computed_text.line_count++;
}

static void wrap_text(TspdfLayout *ctx, TspdfNode *node, double max_width) {
    node->computed_text.line_count = 0;

    const char *text = node->text.text;
    if (!text[0]) return;

    if (node->text.wrap == TSPDF_WRAP_NONE || max_width <= 0) {
        // Handle newlines even in TSPDF_WRAP_NONE mode
        const char *p = text;
        while (*p && node->computed_text.line_count < TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            emit_line(ctx, node, p, len);
            p += len;
            if (nl) p++;  // skip \n
            else break;
        }
        return;
    }

    if (node->text.wrap == TSPDF_WRAP_WORD) {
        // Optimized word-by-word wrapping: O(n) instead of O(n^2)
        // Measure a space character once for accumulation
        double space_width = measure_text(ctx, node->text.font_name,
                                           node->text.font_size, " ");

        const char *p = text;
        while (*p && node->computed_text.line_count < TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES) {
            const char *line_start = p;
            double line_width = 0;
            const char *last_word_end = NULL;    // end of last word that fits
            const char *last_word_resume = NULL; // where to resume after break
            bool first_word_on_line = true;

            while (*p && *p != '\n') {
                // Find next word boundary
                const char *word_start = p;
                while (*p && *p != ' ' && *p != '\n') p++;
                const char *word_end = p;
                size_t word_len = (size_t)(word_end - word_start);

                // Measure this word
                char tmp[TSPDF_LAYOUT_MAX_TEXT];
                if (word_len >= sizeof(tmp)) word_len = sizeof(tmp) - 1;
                memcpy(tmp, word_start, word_len);
                tmp[word_len] = '\0';
                double word_width = measure_text(ctx, node->text.font_name,
                                                  node->text.font_size, tmp);

                double needed = first_word_on_line
                    ? word_width
                    : line_width + space_width + word_width;

                if (!first_word_on_line && needed > max_width) {
                    // This word doesn't fit; break before it
                    // Emit from line_start to last_word_end
                    p = word_start;
                    break;
                }

                // Word fits (or is the first word on line — always accept it)
                line_width = first_word_on_line ? word_width : needed;
                last_word_end = word_end;
                last_word_resume = word_end;
                first_word_on_line = false;

                // Skip spaces after the word
                while (*p == ' ') p++;
            }

            // Determine line end
            const char *line_end;
            if (*p == '\n') {
                line_end = p;
                p++;  // skip newline
            } else if (*p == '\0') {
                line_end = p;
            } else {
                // Broke before a word
                line_end = last_word_end ? last_word_end : p;
                // Skip spaces after break point
                const char *resume = last_word_resume ? last_word_resume : p;
                while (*resume == ' ') resume++;
                p = resume;
            }

            size_t line_len = (size_t)(line_end - line_start);
            // Trim trailing spaces from line
            while (line_len > 0 && line_start[line_len - 1] == ' ') line_len--;
            emit_line(ctx, node, line_start, line_len);

            if (p == line_start && *p && *p != '\n') {
                // Safety: if we made no progress, skip one character to avoid infinite loop
                p++;
            }
        }
        return;
    }

    // TSPDF_WRAP_CHAR: measure codepoint-by-codepoint
    const char *p = text;
    while (*p && node->computed_text.line_count < TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES) {
        const char *line_start = p;
        double line_width = 0;
        const char *last_fit = p;

        while (*p && *p != '\n') {
            // Decode one UTF-8 codepoint to find its byte length
            int cp_len = 1;
            uint8_t b = (uint8_t)*p;
            if (b >= 0xF0) cp_len = 4;
            else if (b >= 0xE0) cp_len = 3;
            else if (b >= 0xC0) cp_len = 2;

            // Validate we have enough bytes (don't read past null terminator)
            int actual_len = 0;
            while (actual_len < cp_len && p[actual_len]) actual_len++;
            if (actual_len < cp_len) cp_len = actual_len ? actual_len : 1;

            // Measure the full codepoint
            char cp_buf[5] = {0};
            for (int i = 0; i < cp_len; i++) cp_buf[i] = p[i];
            double char_w = measure_text(ctx, node->text.font_name,
                                          node->text.font_size, cp_buf);
            if (line_width + char_w > max_width && p > line_start) {
                break;
            }
            line_width += char_w;
            p += cp_len;
            last_fit = p;
        }

        const char *line_end;
        if (*p == '\n') {
            line_end = p;
            p++;
        } else if (*p == '\0') {
            line_end = p;
        } else {
            line_end = last_fit;
            p = last_fit;
        }

        size_t line_len = (size_t)(line_end - line_start);
        emit_line(ctx, node, line_start, line_len);

        if (p == line_start && *p && *p != '\n') {
            // Safety: skip one full codepoint to avoid infinite loop
            uint8_t b = (uint8_t)*p;
            int skip = 1;
            if (b >= 0xF0) skip = 4;
            else if (b >= 0xE0) skip = 3;
            else if (b >= 0xC0) skip = 2;
            for (int s = 0; s < skip && *p; s++) p++;
        }
    }
}

// ============================================================
// PHASE 1: Compute sizes (width and height) for all nodes
// Does NOT set computed_x / computed_y.
// ============================================================

static void compute_sizes(TspdfLayout *ctx, TspdfNode *node, double avail_width, double avail_height);

static double text_content_width(TspdfNode *node) {
    double max_w = 0;
    for (int i = 0; i < node->computed_text.line_count; i++) {
        if (node->computed_text.line_widths[i] > max_w)
            max_w = node->computed_text.line_widths[i];
    }
    return max_w;
}

static double text_content_height(TspdfLayout *ctx, TspdfNode *node) {
    double lh = get_line_height(ctx, node->text.font_name, node->text.font_size);
    lh *= node->text.line_height_factor;
    return node->computed_text.line_count * lh;
}

static void compute_sizes(TspdfLayout *ctx, TspdfNode *node, double avail_width, double avail_height) {
    // --- Resolve width ---
    switch (node->width.mode) {
        case TSPDF_SIZE_FIXED:   node->computed_width = node->width.value; break;
        case TSPDF_SIZE_PERCENT: node->computed_width = avail_width * node->width.value; break;
        case TSPDF_SIZE_GROW:    node->computed_width = avail_width; break;
        case TSPDF_SIZE_FIT:     node->computed_width = 0; break; // resolved below
    }

    // For text nodes, wrap with current width to determine content size
    if (node->type == TSPDF_NODE_TEXT) {
        if (node->text.span_count > 0) {
            // Rich text: measure total span width (single line, no wrapping)
            double total_w = 0;
            double max_fs = 0;
            for (int s = 0; s < node->text.span_count; s++) {
                total_w += measure_text(ctx, node->text.spans[s].font_name,
                                         node->text.spans[s].font_size,
                                         node->text.spans[s].text);
                if (node->text.spans[s].font_size > max_fs)
                    max_fs = node->text.spans[s].font_size;
            }
            if (node->width.mode == TSPDF_SIZE_FIT) {
                node->computed_width = total_w + node->padding.left + node->padding.right;
            }
            double line_h = max_fs * node->text.line_height_factor;
            switch (node->height.mode) {
                case TSPDF_SIZE_FIXED:   node->computed_height = node->height.value; break;
                case TSPDF_SIZE_PERCENT: node->computed_height = avail_height * node->height.value; break;
                case TSPDF_SIZE_GROW:    node->computed_height = avail_height; break;
                case TSPDF_SIZE_FIT:
                    node->computed_height = line_h + node->padding.top + node->padding.bottom;
                    break;
            }
            return;
        }

        double text_avail = (node->width.mode == TSPDF_SIZE_FIT)
            ? avail_width - node->padding.left - node->padding.right
            : node->computed_width - node->padding.left - node->padding.right;
        wrap_text(ctx, node, text_avail > 0 ? text_avail : 1e9);

        if (node->width.mode == TSPDF_SIZE_FIT) {
            node->computed_width = text_content_width(node) + node->padding.left + node->padding.right;
        }

        // Resolve height
        switch (node->height.mode) {
            case TSPDF_SIZE_FIXED:   node->computed_height = node->height.value; break;
            case TSPDF_SIZE_PERCENT: node->computed_height = avail_height * node->height.value; break;
            case TSPDF_SIZE_GROW:    node->computed_height = avail_height; break;
            case TSPDF_SIZE_FIT:
                node->computed_height = text_content_height(ctx, node)
                    + node->padding.top + node->padding.bottom;
                break;
        }
        return;
    }

    // --- Box node: compute children sizes ---
    bool is_row = (node->direction == TSPDF_DIR_ROW);
    double inner_w = node->computed_width - node->padding.left - node->padding.right;
    double inner_h_est = (node->height.mode == TSPDF_SIZE_FIXED)
        ? node->height.value - node->padding.top - node->padding.bottom
        : avail_height - node->padding.top - node->padding.bottom;

    // For FIT width, we need to measure children first with avail_width
    double fit_inner_w = avail_width - node->padding.left - node->padding.right;
    double child_avail_w = (node->width.mode == TSPDF_SIZE_FIT) ? fit_inner_w : inner_w;
    if (child_avail_w < 0) child_avail_w = 0;

    int num_gaps = node->child_count > 1 ? node->child_count - 1 : 0;
    double gap_total = num_gaps * node->gap;

    // First pass: size non-grow children on the main axis
    double total_fixed_main = 0;
    double total_grow_weight = 0;

    for (int i = 0; i < node->child_count; i++) {
        TspdfNode *child = node->children[i];
        TspdfSize child_main = is_row ? child->width : child->height;

        if (child_main.mode == TSPDF_SIZE_GROW) {
            total_grow_weight += child_main.value > 0 ? child_main.value : 1;
        } else {
            // Compute this child's sizes
            double cw = is_row ? child_avail_w : child_avail_w;
            double ch = is_row ? inner_h_est : inner_h_est;
            compute_sizes(ctx, child, cw, ch);
            double child_main_size = is_row ? child->computed_width : child->computed_height;
            total_fixed_main += child_main_size;
        }
    }

    // If width is FIT, resolve it now from children
    if (node->width.mode == TSPDF_SIZE_FIT) {
        if (is_row) {
            node->computed_width = total_fixed_main + gap_total
                + node->padding.left + node->padding.right;
            // Grow children in a FIT container get 0 width
        } else {
            // Cross axis: take widest child
            double max_child_w = 0;
            for (int i = 0; i < node->child_count; i++) {
                if (node->children[i]->computed_width > max_child_w)
                    max_child_w = node->children[i]->computed_width;
            }
            node->computed_width = max_child_w + node->padding.left + node->padding.right;
        }
        inner_w = node->computed_width - node->padding.left - node->padding.right;
        child_avail_w = inner_w;
    }

    // Resolve height (non-FIT modes first; FIT is deferred until after all children are sized)
    bool height_is_fit = (node->height.mode == TSPDF_SIZE_FIT);
    switch (node->height.mode) {
        case TSPDF_SIZE_FIXED:   node->computed_height = node->height.value; break;
        case TSPDF_SIZE_PERCENT: node->computed_height = avail_height * node->height.value; break;
        case TSPDF_SIZE_GROW:    node->computed_height = avail_height; break;
        case TSPDF_SIZE_FIT: {
            if (!is_row) {
                // Column: main axis is height, all non-GROW children are already sized
                node->computed_height = total_fixed_main + gap_total
                    + node->padding.top + node->padding.bottom;
            } else {
                // Row: height depends on tallest child, but GROW-width children
                // haven't been sized yet. Use avail_height as estimate for now.
                node->computed_height = avail_height;
            }
            break;
        }
    }

    double inner_h = node->computed_height - node->padding.top - node->padding.bottom;
    if (inner_h < 0) inner_h = 0;

    // Second pass: resolve GROW children on the main axis
    double main_total = is_row ? inner_w : inner_h;
    double remaining = main_total - total_fixed_main - gap_total;
    if (remaining < 0) remaining = 0;

    for (int i = 0; i < node->child_count; i++) {
        TspdfNode *child = node->children[i];
        TspdfSize child_main = is_row ? child->width : child->height;

        if (child_main.mode == TSPDF_SIZE_GROW) {
            double weight = child_main.value > 0 ? child_main.value : 1;
            double share = (total_grow_weight > 0)
                ? remaining * (weight / total_grow_weight) : 0;

            if (is_row) {
                compute_sizes(ctx, child, share, inner_h);
                child->computed_width = share;
            } else {
                compute_sizes(ctx, child, inner_w, share);
                child->computed_height = share;
            }
        }
    }

    // Now that ALL children are sized, resolve FIT height for rows
    if (height_is_fit && is_row) {
        double max_child_h = 0;
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]->computed_height > max_child_h)
                max_child_h = node->children[i]->computed_height;
        }
        node->computed_height = max_child_h + node->padding.top + node->padding.bottom;
        inner_h = node->computed_height - node->padding.top - node->padding.bottom;
    }

    // Cross-axis grow: now that both dimensions are final
    for (int i = 0; i < node->child_count; i++) {
        TspdfNode *child = node->children[i];
        TspdfSize child_cross = is_row ? child->height : child->width;
        if (child_cross.mode == TSPDF_SIZE_GROW) {
            if (is_row)
                child->computed_height = inner_h;
            else
                child->computed_width = inner_w;
        }
    }
}

// ============================================================
// PHASE 2: Position all nodes (set computed_x / computed_y)
// Runs AFTER all sizes are known.
// ============================================================

static void compute_positions(TspdfNode *node) {
    if (node->type == TSPDF_NODE_TEXT || node->child_count == 0) return;

    bool is_row = (node->direction == TSPDF_DIR_ROW);
    double inner_w = node->computed_width - node->padding.left - node->padding.right;
    double inner_h = node->computed_height - node->padding.top - node->padding.bottom;

    // Compute total main-axis size of children
    double total_main = 0;
    for (int i = 0; i < node->child_count; i++) {
        total_main += is_row ? node->children[i]->computed_width
                             : node->children[i]->computed_height;
        if (i > 0) total_main += node->gap;
    }

    // Main axis alignment offset
    TspdfAlignment main_align = is_row ? node->align_x : node->align_y;
    double main_space = (is_row ? inner_w : inner_h) - total_main;
    if (main_space < 0) main_space = 0;
    double cursor = 0;
    switch (main_align) {
        case TSPDF_ALIGN_START:  cursor = 0; break;
        case TSPDF_ALIGN_CENTER: cursor = main_space / 2; break;
        case TSPDF_ALIGN_END:    cursor = main_space; break;
    }

    for (int i = 0; i < node->child_count; i++) {
        TspdfNode *child = node->children[i];

        double child_main  = is_row ? child->computed_width  : child->computed_height;
        double child_cross = is_row ? child->computed_height : child->computed_width;
        double cross_total = is_row ? inner_h : inner_w;
        double cross_space = cross_total - child_cross;
        if (cross_space < 0) cross_space = 0;

        TspdfAlignment cross_align = is_row ? node->align_y : node->align_x;
        double cross_offset = 0;
        switch (cross_align) {
            case TSPDF_ALIGN_START:  cross_offset = 0; break;
            case TSPDF_ALIGN_CENTER: cross_offset = cross_space / 2; break;
            case TSPDF_ALIGN_END:    cross_offset = cross_space; break;
        }

        if (is_row) {
            child->computed_x = node->computed_x + node->padding.left + cursor;
            child->computed_y = node->computed_y + node->padding.top + cross_offset;
        } else {
            child->computed_x = node->computed_x + node->padding.left + cross_offset;
            child->computed_y = node->computed_y + node->padding.top + cursor;
        }

        cursor += child_main + node->gap;

        // Recurse into child
        compute_positions(child);
    }
}

void tspdf_layout_compute(TspdfLayout *ctx, TspdfNode *root, double root_width, double root_height) {
    // Phase 1: sizes
    compute_sizes(ctx, root, root_width, root_height);

    // Phase 2: positions
    root->computed_x = 0;
    root->computed_y = 0;
    compute_positions(root);
}

// --- Rendering ---

static void render_show_text(TspdfLayout *ctx, TspdfStream *stream,
                              const char *font_name, const char *text) {
    if (ctx->doc) {
        TspdfWriter *doc = (TspdfWriter *)ctx->doc;
        TTF_Font *ttf = tspdf_writer_get_ttf(doc, font_name);
        if (ttf) {
            TspdfFont *pdf_font = tspdf_writer_get_font(doc, font_name);
            tspdf_stream_show_text_utf8(stream, text, ttf, pdf_font);
            return;
        }
    }
    tspdf_stream_show_text(stream, text);
}

static void render_node(TspdfLayout *ctx, TspdfNode *node, TspdfStream *stream, double page_height) {
    // Convert from top-left layout coords to PDF bottom-left coords
    // In layout: (x, y) is top-left of element, y increases downward
    // In PDF: (x, y) is bottom-left, y increases upward
    // So: pdf_bottom_y = page_height - layout_y - height
    double pdf_x = node->computed_x;
    double pdf_y = page_height - node->computed_y - node->computed_height;
    double w = node->computed_width;
    double h = node->computed_height;

    // Determine effective scale (0 means default 1.0)
    double sx = (node->scale_x != 0.0) ? node->scale_x : 1.0;
    double sy = (node->scale_y != 0.0) ? node->scale_y : 1.0;
    bool has_transform = (node->rotation != 0.0 || sx != 1.0 || sy != 1.0);

    if (has_transform) {
        tspdf_stream_save(stream);
        // Center of element in PDF coords
        double cx = pdf_x + w / 2.0;
        double cy = pdf_y + h / 2.0;
        double rad = node->rotation * M_PI / 180.0;
        double cosA = cos(rad);
        double sinA = sin(rad);
        double a = sx * cosA;
        double b = sx * sinA;
        double c = -sy * sinA;
        double d = sy * cosA;
        double e = cx - a * cx - c * cy;
        double f = cy - b * cx - d * cy;
        tspdf_stream_concat_matrix(stream, a, b, c, d, e, f);
    }

    // Drop shadow (drawn before background so it appears behind)
    if (node->style && node->style->has_shadow) {
        double sx = pdf_x + node->style->shadow_offset_x;
        // shadow_offset_y is positive=down in layout coords, but PDF y is up
        double sy = pdf_y - node->style->shadow_offset_y;

        bool has_radius = node->style->corner_radius[0] > 0 ||
                          node->style->corner_radius[1] > 0 ||
                          node->style->corner_radius[2] > 0 ||
                          node->style->corner_radius[3] > 0;

        // Simulate blur with multiple expanding, increasingly transparent layers
        int layers = (node->style->shadow_blur > 0) ? 4 : 1;
        for (int layer = layers - 1; layer >= 0; layer--) {
            double expand = (layers > 1) ? (node->style->shadow_blur * (layer + 1) / layers) : 0;
            double alpha = node->style->shadow_color.a / (layer + 1);

            TspdfColor sc = node->style->shadow_color;
            sc.a = alpha;
            // Blend shadow color toward background (approximate opacity via lighter color)
            TspdfColor blended = {
                sc.r + (1.0 - sc.r) * (1.0 - alpha),
                sc.g + (1.0 - sc.g) * (1.0 - alpha),
                sc.b + (1.0 - sc.b) * (1.0 - alpha),
                1.0
            };
            tspdf_stream_set_fill_color(stream, blended);

            if (has_radius) {
                double tl = node->style->corner_radius[0];
                double tr = node->style->corner_radius[1];
                double br = node->style->corner_radius[2];
                double bl = node->style->corner_radius[3];
                tspdf_stream_rounded_rect_pdf(stream,
                    sx - expand, sy - expand,
                    w + expand * 2, h + expand * 2,
                    tl + expand, tr + expand, br + expand, bl + expand);
            } else {
                tspdf_stream_rect(stream, sx - expand, sy - expand,
                                w + expand * 2, h + expand * 2);
            }
            tspdf_stream_fill(stream);
        }
    }

    // Background
    if (node->style && node->style->has_background) {
        tspdf_stream_set_fill_color(stream, node->style->background);

        bool has_radius = node->style->corner_radius[0] > 0 ||
                          node->style->corner_radius[1] > 0 ||
                          node->style->corner_radius[2] > 0 ||
                          node->style->corner_radius[3] > 0;

        if (has_radius) {
            double tl = node->style->corner_radius[0];
            double tr = node->style->corner_radius[1];
            double br = node->style->corner_radius[2];
            double bl = node->style->corner_radius[3];
            tspdf_stream_rounded_rect_pdf(stream, pdf_x, pdf_y, w, h, tl, tr, br, bl);
        } else {
            tspdf_stream_rect(stream, pdf_x, pdf_y, w, h);
        }
        tspdf_stream_fill(stream);
    }

    // Background image
    if (node->style && node->style->bg_image) {
        bool has_radius = node->style->corner_radius[0] > 0 ||
                          node->style->corner_radius[1] > 0 ||
                          node->style->corner_radius[2] > 0 ||
                          node->style->corner_radius[3] > 0;
        tspdf_stream_save(stream);
        // Clip to box bounds (with rounded corners if applicable)
        if (has_radius) {
            double tl = node->style->corner_radius[0];
            double tr = node->style->corner_radius[1];
            double br = node->style->corner_radius[2];
            double bl = node->style->corner_radius[3];
            tspdf_stream_rounded_rect_pdf(stream, pdf_x, pdf_y, w, h, tl, tr, br, bl);
        } else {
            tspdf_stream_rect(stream, pdf_x, pdf_y, w, h);
        }
        tspdf_stream_clip(stream);
        tspdf_stream_draw_image(stream, node->style->bg_image, pdf_x, pdf_y, w, h);
        tspdf_stream_restore(stream);
    }

    // Apply dash pattern for borders
    if (node->style && node->style->dash_on > 0) {
        double dash[] = {node->style->dash_on, node->style->dash_off > 0 ? node->style->dash_off : node->style->dash_on};
        tspdf_stream_set_dash(stream, dash, 2, 0);
    }

    // Border (all sides)
    if (node->style && node->style->has_border && node->style->border_width > 0) {
        tspdf_stream_set_stroke_color(stream, node->style->border_color);
        tspdf_stream_set_line_width(stream, node->style->border_width);

        bool has_radius = node->style->corner_radius[0] > 0 ||
                          node->style->corner_radius[1] > 0 ||
                          node->style->corner_radius[2] > 0 ||
                          node->style->corner_radius[3] > 0;

        if (has_radius) {
            double tl = node->style->corner_radius[0];
            double tr = node->style->corner_radius[1];
            double br = node->style->corner_radius[2];
            double bl = node->style->corner_radius[3];
            tspdf_stream_rounded_rect_pdf(stream, pdf_x, pdf_y, w, h, tl, tr, br, bl);
        } else {
            tspdf_stream_rect(stream, pdf_x, pdf_y, w, h);
        }
        tspdf_stream_stroke(stream);
    }

    // Per-side borders
    if (node->style && node->style->has_border_top) {
        tspdf_stream_set_stroke_color(stream, node->style->border_color_top);
        tspdf_stream_set_line_width(stream, node->style->border_top);
        tspdf_stream_move_to(stream, pdf_x, pdf_y + h);
        tspdf_stream_line_to(stream, pdf_x + w, pdf_y + h);
        tspdf_stream_stroke(stream);
    }
    if (node->style && node->style->has_border_bottom) {
        tspdf_stream_set_stroke_color(stream, node->style->border_color_bottom);
        tspdf_stream_set_line_width(stream, node->style->border_bottom);
        tspdf_stream_move_to(stream, pdf_x, pdf_y);
        tspdf_stream_line_to(stream, pdf_x + w, pdf_y);
        tspdf_stream_stroke(stream);
    }
    if (node->style && node->style->has_border_left) {
        tspdf_stream_set_stroke_color(stream, node->style->border_color_left);
        tspdf_stream_set_line_width(stream, node->style->border_left);
        tspdf_stream_move_to(stream, pdf_x, pdf_y);
        tspdf_stream_line_to(stream, pdf_x, pdf_y + h);
        tspdf_stream_stroke(stream);
    }
    if (node->style && node->style->has_border_right) {
        tspdf_stream_set_stroke_color(stream, node->style->border_color_right);
        tspdf_stream_set_line_width(stream, node->style->border_right);
        tspdf_stream_move_to(stream, pdf_x + w, pdf_y);
        tspdf_stream_line_to(stream, pdf_x + w, pdf_y + h);
        tspdf_stream_stroke(stream);
    }

    // Reset dash pattern after borders
    if (node->style && node->style->dash_on > 0) {
        tspdf_stream_set_dash(stream, NULL, 0, 0);
    }

    // Text rendering
    if (node->type == TSPDF_NODE_TEXT && node->text.span_count > 0) {
        // Rich text: render spans inline on one line
        double max_font_size = 0;
        for (int s = 0; s < node->text.span_count; s++) {
            if (node->text.spans[s].font_size > max_font_size)
                max_font_size = node->text.spans[s].font_size;
        }
        double ascent = max_font_size * TSPDF_LAYOUT_DEFAULT_ASCENT_FACTOR;
        double baseline_from_top = node->padding.top + ascent;
        double text_pdf_y = page_height - (node->computed_y + baseline_from_top);
        double cursor_x = pdf_x + node->padding.left;

        // TspdfAlignment: compute total width
        double total_w = 0;
        for (int s = 0; s < node->text.span_count; s++) {
            total_w += measure_text(ctx, node->text.spans[s].font_name,
                                     node->text.spans[s].font_size,
                                     node->text.spans[s].text);
        }
        double avail_w = node->computed_width - node->padding.left - node->padding.right;
        switch (node->text.alignment) {
            case TSPDF_TEXT_ALIGN_LEFT:   break;
            case TSPDF_TEXT_ALIGN_CENTER: cursor_x += (avail_w - total_w) / 2; break;
            case TSPDF_TEXT_ALIGN_RIGHT:  cursor_x += avail_w - total_w; break;
        }

        for (int s = 0; s < node->text.span_count; s++) {
            TspdfTextSpan *span = &node->text.spans[s];
            double span_w = measure_text(ctx, span->font_name, span->font_size, span->text);

            tspdf_stream_set_fill_color(stream, span->color);
            tspdf_stream_begin_text(stream);
            tspdf_stream_set_font(stream, span->font_name, span->font_size);
            tspdf_stream_text_position(stream, cursor_x, text_pdf_y);
            render_show_text(ctx, stream, span->font_name, span->text);
            tspdf_stream_end_text(stream);

            // Span decorations
            if (span->decoration) {
                tspdf_stream_set_stroke_color(stream, span->color);
                double dt = span->font_size * 0.05;
                if (dt < 0.5) dt = 0.5;
                tspdf_stream_set_line_width(stream, dt);

                if (span->decoration & TSPDF_TEXT_DECOR_UNDERLINE) {
                    double uy = text_pdf_y - span->font_size * 0.15;
                    tspdf_stream_move_to(stream, cursor_x, uy);
                    tspdf_stream_line_to(stream, cursor_x + span_w, uy);
                    tspdf_stream_stroke(stream);
                }
                if (span->decoration & TSPDF_TEXT_DECOR_STRIKETHROUGH) {
                    double sy = text_pdf_y + span->font_size * 0.25;
                    tspdf_stream_move_to(stream, cursor_x, sy);
                    tspdf_stream_line_to(stream, cursor_x + span_w, sy);
                    tspdf_stream_stroke(stream);
                }
            }
            cursor_x += span_w;
        }
    } else if (node->type == TSPDF_NODE_TEXT && node->computed_text.line_count > 0) {
        double lh = get_line_height(ctx, node->text.font_name, node->text.font_size);
        lh *= node->text.line_height_factor;
        double ascent = node->text.font_size * TSPDF_LAYOUT_DEFAULT_ASCENT_FACTOR;

        tspdf_stream_set_fill_color(stream, node->text.color);

        for (int i = 0; i < node->computed_text.line_count; i++) {
            // In layout coords: text baseline from top
            double baseline_from_top = node->padding.top + i * lh + ascent;
            // Convert to PDF y
            double text_pdf_y = page_height - (node->computed_y + baseline_from_top);

            double line_x = pdf_x + node->padding.left;

            // Text alignment
            double text_w = node->computed_text.line_widths[i];
            double avail_w = node->computed_width - node->padding.left - node->padding.right;

            switch (node->text.alignment) {
                case TSPDF_TEXT_ALIGN_LEFT:   break;
                case TSPDF_TEXT_ALIGN_CENTER: line_x += (avail_w - text_w) / 2; break;
                case TSPDF_TEXT_ALIGN_RIGHT:  line_x += avail_w - text_w; break;
            }

            tspdf_stream_begin_text(stream);
            tspdf_stream_set_font(stream, node->text.font_name, node->text.font_size);
            tspdf_stream_text_position(stream, line_x, text_pdf_y);
            render_show_text(ctx, stream, node->text.font_name, node->computed_text.lines[i]);
            tspdf_stream_end_text(stream);

            // Text decorations (drawn as lines)
            if (node->text.decoration) {
                tspdf_stream_set_stroke_color(stream, node->text.color);
                double decor_thickness = node->text.font_size * 0.05;
                if (decor_thickness < 0.5) decor_thickness = 0.5;
                tspdf_stream_set_line_width(stream, decor_thickness);

                if (node->text.decoration & TSPDF_TEXT_DECOR_UNDERLINE) {
                    double uy = text_pdf_y - node->text.font_size * 0.15;
                    tspdf_stream_move_to(stream, line_x, uy);
                    tspdf_stream_line_to(stream, line_x + text_w, uy);
                    tspdf_stream_stroke(stream);
                }
                if (node->text.decoration & TSPDF_TEXT_DECOR_STRIKETHROUGH) {
                    double sy = text_pdf_y + node->text.font_size * 0.25;
                    tspdf_stream_move_to(stream, line_x, sy);
                    tspdf_stream_line_to(stream, line_x + text_w, sy);
                    tspdf_stream_stroke(stream);
                }
                if (node->text.decoration & TSPDF_TEXT_DECOR_OVERLINE) {
                    double oy = text_pdf_y + node->text.font_size * TSPDF_LAYOUT_DEFAULT_ASCENT_FACTOR;
                    tspdf_stream_move_to(stream, line_x, oy);
                    tspdf_stream_line_to(stream, line_x + text_w, oy);
                    tspdf_stream_stroke(stream);
                }
            }
        }
    }

    // Vector path rendering
    if (node->path && node->path->count > 0) {
        TspdfPathConfig *p = node->path;
        tspdf_stream_save(stream);

        // Clip path rendering to node bounds
        tspdf_stream_rect(stream, pdf_x, pdf_y, w, h);
        tspdf_stream_clip(stream);

        if (p->has_fill) tspdf_stream_set_fill_color(stream, p->fill_color);
        if (p->has_stroke) {
            tspdf_stream_set_stroke_color(stream, p->stroke_color);
            tspdf_stream_set_line_width(stream, p->stroke_width);
        }

        for (int i = 0; i < p->count; i++) {
            TspdfPathCmd *cmd = &p->commands[i];
            // Convert from node-relative y-down to PDF absolute y-up coords
            double cx = pdf_x + cmd->x;
            double cy = pdf_y + h - cmd->y;  // flip y

            switch (cmd->type) {
                case TSPDF_PATH_MOVE_TO:
                    tspdf_stream_move_to(stream, cx, cy);
                    break;
                case TSPDF_PATH_LINE_TO:
                    tspdf_stream_line_to(stream, cx, cy);
                    break;
                case TSPDF_PATH_CURVE_TO: {
                    double c1x = pdf_x + cmd->cx1;
                    double c1y = pdf_y + h - cmd->cy1;
                    double c2x = pdf_x + cmd->cx2;
                    double c2y = pdf_y + h - cmd->cy2;
                    tspdf_stream_curve_to(stream, c1x, c1y, c2x, c2y, cx, cy);
                    break;
                }
                case TSPDF_PATH_CLOSE:
                    tspdf_stream_close_path(stream);
                    break;
                case TSPDF_PATH_ARC: {
                    // Convert arc to cubic Bezier segments
                    // cx,cy here is center in PDF coords
                    double acx = pdf_x + cmd->x;
                    double acy = pdf_y + h - cmd->y;
                    double r = cmd->radius;
                    double start = cmd->start_angle * M_PI / 180.0;
                    double sweep = cmd->sweep_angle * M_PI / 180.0;

                    // Split arc into 90-degree (or less) segments
                    int segments = (int)(fabs(sweep) / (M_PI / 2.0)) + 1;
                    double seg_sweep = sweep / segments;

                    for (int s = 0; s < segments; s++) {
                        double a0 = start + s * seg_sweep;
                        double a1 = a0 + seg_sweep;
                        // Approximate arc with cubic Bezier
                        double alpha = 4.0 * tan(seg_sweep / 4.0) / 3.0;
                        double cos0 = cos(a0), sin0 = sin(a0);
                        double cos1 = cos(a1), sin1 = sin(a1);
                        double p0x = acx + r * cos0, p0y = acy + r * sin0;
                        double p3x = acx + r * cos1, p3y = acy + r * sin1;
                        double p1x = p0x - r * alpha * sin0;
                        double p1y = p0y + r * alpha * cos0;
                        double p2x = p3x + r * alpha * sin1;
                        double p2y = p3y - r * alpha * cos1;
                        if (s == 0)
                            tspdf_stream_move_to(stream, p0x, p0y);
                        tspdf_stream_curve_to(stream, p1x, p1y, p2x, p2y, p3x, p3y);
                    }
                    break;
                }
            }
        }

        if (p->has_fill && p->has_stroke)
            tspdf_stream_fill_stroke(stream);
        else if (p->has_fill)
            tspdf_stream_fill(stream);
        else if (p->has_stroke)
            tspdf_stream_stroke(stream);

        tspdf_stream_restore(stream);
    }

    // Clipping: if clip_children is set, establish a clipping rectangle
    bool did_clip = false;
    if (node->clip_children && node->child_count > 0) {
        tspdf_stream_save(stream);
        tspdf_stream_rect(stream, pdf_x, pdf_y, w, h);
        tspdf_stream_clip(stream);
        did_clip = true;
    }

    // Render children
    for (int i = 0; i < node->child_count; i++) {
        render_node(ctx, node->children[i], stream, page_height);
    }

    // End clipping scope
    if (did_clip) {
        tspdf_stream_restore(stream);
    }

    // End transform scope
    if (has_transform) {
        tspdf_stream_restore(stream);
    }
}

// --- Table helper ---

TspdfNode *tspdf_layout_table(TspdfLayout *ctx, const char **columns, const double *col_widths, int col_count, TspdfTableStyle style) {
    TspdfNode *table = tspdf_layout_box(ctx);
    table->width = tspdf_size_grow();
    table->height = tspdf_size_fit();
    table->direction = TSPDF_DIR_COLUMN;

    if (style.border_width > 0) {
        ensure_style(ctx, table)->has_border = true;
        table->style->border_color = style.border_color;
        table->style->border_width = style.border_width;
    }

    // Header row
    TspdfNode *hdr = tspdf_layout_box(ctx);
    hdr->width = tspdf_size_grow();
    hdr->height = tspdf_size_fixed(style.header_height > 0 ? style.header_height : 32);
    hdr->direction = TSPDF_DIR_ROW;
    hdr->align_y = TSPDF_ALIGN_CENTER;
    ensure_style(ctx, hdr)->has_background = true;
    hdr->style->background = style.header_bg;
    hdr->repeat_mode = TSPDF_REPEAT_ALL;
    tspdf_layout_add_child(table, hdr);

    for (int i = 0; i < col_count; i++) {
        TspdfNode *cell = tspdf_layout_box(ctx);
        cell->width = tspdf_size_percent(col_widths[i]);
        cell->height = tspdf_size_grow();
        cell->align_x = TSPDF_ALIGN_CENTER;
        cell->align_y = TSPDF_ALIGN_CENTER;
        cell->padding = tspdf_padding_xy(style.padding, 0);
        tspdf_layout_add_child(hdr, cell);

        TspdfNode *t = tspdf_layout_text(ctx, columns[i], style.font_name, style.font_size);
        t->text.color = style.header_text_color;
        tspdf_layout_add_child(cell, t);
    }

    return table;
}

void tspdf_layout_table_add_row(TspdfLayout *ctx, TspdfNode *table, const char **cells, const double *col_widths,
                           int cell_count, TspdfTableStyle style) {
    int data_row_index = table->child_count - 1;

    TspdfNode *row = tspdf_layout_box(ctx);
    row->width = tspdf_size_grow();
    if (style.row_height > 0)
        row->height = tspdf_size_fixed(style.row_height);
    else
        row->height = tspdf_size_fit();
    row->direction = TSPDF_DIR_ROW;
    row->align_y = TSPDF_ALIGN_CENTER;
    ensure_style(ctx, row)->has_background = true;
    row->style->background = (data_row_index % 2 == 0) ? style.row_bg_even : style.row_bg_odd;

    if (style.border_width > 0) {
        row->style->has_border_bottom = true;
        row->style->border_bottom = style.border_width * 0.5;
        row->style->border_color_bottom = style.border_color;
    }

    tspdf_layout_add_child(table, row);

    for (int i = 0; i < cell_count; i++) {
        TspdfNode *cell = tspdf_layout_box(ctx);
        cell->width = tspdf_size_percent(col_widths[i]);
        cell->height = (style.row_height > 0) ? tspdf_size_grow() : tspdf_size_fit();
        cell->align_x = TSPDF_ALIGN_START;
        cell->align_y = TSPDF_ALIGN_CENTER;
        cell->padding = tspdf_padding_xy(style.padding, style.row_height > 0 ? 0 : style.padding);
        tspdf_layout_add_child(row, cell);

        TspdfNode *t = tspdf_layout_text(ctx, cells[i], style.font_name, style.font_size);
        t->text.color = style.text_color;
        t->width = tspdf_size_grow();
        if (style.row_height <= 0) t->text.wrap = TSPDF_WRAP_WORD;
        tspdf_layout_add_child(cell, t);
    }
}

void tspdf_layout_table_add_row_spans(TspdfLayout *ctx, TspdfNode *table, const char **cells, const int *colspans,
                                 int cell_count, const double *col_widths, int col_count,
                                 TspdfTableStyle style) {
    int data_row_index = table->child_count - 1;
    (void)col_count;

    TspdfNode *row = tspdf_layout_box(ctx);
    row->width = tspdf_size_grow();
    if (style.row_height > 0)
        row->height = tspdf_size_fixed(style.row_height);
    else
        row->height = tspdf_size_fit();
    row->direction = TSPDF_DIR_ROW;
    row->align_y = TSPDF_ALIGN_CENTER;
    ensure_style(ctx, row)->has_background = true;
    row->style->background = (data_row_index % 2 == 0) ? style.row_bg_even : style.row_bg_odd;

    if (style.border_width > 0) {
        row->style->has_border_bottom = true;
        row->style->border_bottom = style.border_width * 0.5;
        row->style->border_color_bottom = style.border_color;
    }

    tspdf_layout_add_child(table, row);

    int col_idx = 0;
    for (int i = 0; i < cell_count; i++) {
        int span = colspans[i];
        // Sum the widths of the spanned columns
        double total_pct = 0;
        for (int j = 0; j < span && col_idx + j < col_count; j++) {
            total_pct += col_widths[col_idx + j];
        }

        TspdfNode *cell = tspdf_layout_box(ctx);
        cell->width = tspdf_size_percent(total_pct);
        cell->height = (style.row_height > 0) ? tspdf_size_grow() : tspdf_size_fit();
        cell->align_x = (span > 1) ? TSPDF_ALIGN_CENTER : TSPDF_ALIGN_START;
        cell->align_y = TSPDF_ALIGN_CENTER;
        cell->padding = tspdf_padding_xy(style.padding, style.row_height > 0 ? 0 : style.padding);
        tspdf_layout_add_child(row, cell);

        TspdfNode *t = tspdf_layout_text(ctx, cells[i], style.font_name, style.font_size);
        t->text.color = style.text_color;
        t->width = tspdf_size_grow();
        if (style.row_height <= 0) t->text.wrap = TSPDF_WRAP_WORD;
        tspdf_layout_add_child(cell, t);

        col_idx += span;
    }
}

// --- Auto-sizing table columns ---

void tspdf_layout_table_compute_widths(TspdfLayout *ctx, const char **headers,
                                  const char **rows_flat, int row_count,
                                  int col_count, TspdfTableStyle style,
                                  double *col_widths_out) {
    if (col_count <= 0 || col_count > TSPDF_LAYOUT_MAX_CHILDREN) return;
    double max_w[TSPDF_LAYOUT_MAX_CHILDREN];
    for (int j = 0; j < col_count; j++) max_w[j] = 0;

    double pad = style.padding * 2;

    // Measure header cells
    for (int j = 0; j < col_count; j++) {
        double w = measure_text(ctx, style.font_name, style.font_size, headers[j]) + pad;
        if (w > max_w[j]) max_w[j] = w;
    }

    // Measure all data rows
    for (int r = 0; r < row_count; r++) {
        for (int j = 0; j < col_count; j++) {
            const char *cell = rows_flat[r * col_count + j];
            double w = measure_text(ctx, style.font_name, style.font_size, cell) + pad;
            if (w > max_w[j]) max_w[j] = w;
        }
    }

    // Normalize to proportional widths summing to 1.0
    double total = 0;
    for (int j = 0; j < col_count; j++) {
        if (max_w[j] < 20) max_w[j] = 20;  // minimum column width
        total += max_w[j];
    }

    if (total <= 0) total = 1;
    for (int j = 0; j < col_count; j++) {
        col_widths_out[j] = max_w[j] / total;
    }
}

TspdfNode *tspdf_layout_table_auto(TspdfLayout *ctx, const char **headers,
                               const char **rows_flat, int row_count,
                               int col_count, TspdfTableStyle style) {
    double col_widths[TSPDF_LAYOUT_MAX_CHILDREN];
    tspdf_layout_table_compute_widths(ctx, headers, rows_flat, row_count,
                                 col_count, style, col_widths);

    TspdfNode *tbl = tspdf_layout_table(ctx, headers, col_widths, col_count, style);

    for (int r = 0; r < row_count; r++) {
        const char **row = &rows_flat[r * col_count];
        tspdf_layout_table_add_row(ctx, tbl, row, col_widths, col_count, style);
    }

    return tbl;
}

// --- List helper ---

TspdfNode *tspdf_layout_list(TspdfLayout *ctx, TspdfListType type, const char *font_name, double font_size, TspdfColor color) {
    TspdfNode *list = tspdf_layout_box(ctx);
    list->width = tspdf_size_grow();
    list->height = tspdf_size_fit();
    list->direction = TSPDF_DIR_COLUMN;
    list->list_type = type;
    list->gap = 4.0;

    list->text.font_name = arena_strdup(ctx->arena, font_name);
    list->text.font_size = font_size;
    list->text.color = color;

    return list;
}

void tspdf_layout_list_add_item(TspdfLayout *ctx, TspdfNode *list, const char *text) {
    bool is_numbered = (list->list_type == TSPDF_LIST_NUMBERED);
    int item_num = list->child_count + 1;

    const char *font_name = list->text.font_name;
    double font_size = list->text.font_size;
    TspdfColor color = list->text.color;

    TspdfNode *row = tspdf_layout_box(ctx);
    row->width = tspdf_size_grow();
    row->height = tspdf_size_fit();
    row->direction = TSPDF_DIR_ROW;
    row->gap = 4;
    row->padding = tspdf_padding_each(2, 0, 2, 0);
    tspdf_layout_add_child(list, row);

    char marker[16];
    if (is_numbered) {
        snprintf(marker, sizeof(marker), "%d.", item_num);
    } else {
        snprintf(marker, sizeof(marker), "\xb7");  // middle dot (WinAnsiEncoding)
    }

    TspdfNode *marker_node = tspdf_layout_text(ctx, marker, font_name, font_size);
    marker_node->text.color = color;
    marker_node->width = tspdf_size_fixed(20);
    marker_node->text.alignment = TSPDF_TEXT_ALIGN_RIGHT;
    tspdf_layout_add_child(row, marker_node);

    TspdfNode *text_node = tspdf_layout_text(ctx, text, font_name, font_size);
    text_node->text.color = color;
    text_node->width = tspdf_size_grow();
    text_node->text.wrap = TSPDF_WRAP_WORD;
    tspdf_layout_add_child(row, text_node);
}

// --- Vector path builder ---

TspdfPathConfig *tspdf_layout_path_begin(TspdfLayout *ctx, TspdfNode *node) {
    TspdfPathConfig *p = (TspdfPathConfig *)tspdf_arena_alloc(ctx->arena, sizeof(TspdfPathConfig));
    if (!p) return NULL;
    memset(p, 0, sizeof(TspdfPathConfig));
    node->path = p;
    return p;
}

bool tspdf_layout_path_move_to(TspdfPathConfig *p, double x, double y) {
    if (p->count >= TSPDF_PATH_MAX_COMMANDS) return false;
    p->commands[p->count++] = (TspdfPathCmd){.type = TSPDF_PATH_MOVE_TO, .x = x, .y = y};
    return true;
}

bool tspdf_layout_path_line_to(TspdfPathConfig *p, double x, double y) {
    if (p->count >= TSPDF_PATH_MAX_COMMANDS) return false;
    p->commands[p->count++] = (TspdfPathCmd){.type = TSPDF_PATH_LINE_TO, .x = x, .y = y};
    return true;
}

bool tspdf_layout_path_curve_to(TspdfPathConfig *p, double x1, double y1, double x2, double y2, double x3, double y3) {
    if (p->count >= TSPDF_PATH_MAX_COMMANDS) return false;
    p->commands[p->count++] = (TspdfPathCmd){
        .type = TSPDF_PATH_CURVE_TO, .x = x3, .y = y3,
        .cx1 = x1, .cy1 = y1, .cx2 = x2, .cy2 = y2
    };
    return true;
}

bool tspdf_layout_path_close(TspdfPathConfig *p) {
    if (p->count >= TSPDF_PATH_MAX_COMMANDS) return false;
    p->commands[p->count++] = (TspdfPathCmd){.type = TSPDF_PATH_CLOSE};
    return true;
}

bool tspdf_layout_path_arc(TspdfPathConfig *p, double cx, double cy, double radius, double start_deg, double sweep_deg) {
    if (p->count >= TSPDF_PATH_MAX_COMMANDS) return false;
    p->commands[p->count++] = (TspdfPathCmd){
        .type = TSPDF_PATH_ARC, .x = cx, .y = cy,
        .radius = radius, .start_angle = start_deg, .sweep_angle = sweep_deg
    };
    return true;
}

void tspdf_layout_path_set_fill(TspdfPathConfig *p, TspdfColor color) {
    p->fill_color = color;
    p->has_fill = true;
}

void tspdf_layout_path_set_stroke(TspdfPathConfig *p, TspdfColor color, double width) {
    p->stroke_color = color;
    p->stroke_width = width;
    p->has_stroke = true;
}

// --- Rendering ---

void tspdf_layout_render(TspdfLayout *ctx, TspdfNode *root, TspdfStream *stream) {
    double page_height = root->computed_height;
    render_node(ctx, root, stream, page_height);
}

// --- Paginated layout ---

int tspdf_layout_compute_paginated(TspdfLayout *ctx, TspdfNode *root, double page_width, double page_height,
                              TspdfPaginationResult *result) {
    memset(result, 0, sizeof(*result));
    result->page_width = page_width;
    result->page_height = page_height;

    // Compute sizes with unlimited height so content gets its natural size
    compute_sizes(ctx, root, page_width, 1e9);
    root->computed_width = page_width;

    // Separate repeating vs content children
    double repeat_h = 0;
    for (int i = 0; i < root->child_count; i++) {
        TspdfNode *child = root->children[i];
        if (child->repeat_mode != TSPDF_REPEAT_NONE) {
            result->repeat_indices[result->repeat_count++] = i;
            if (result->repeat_count > 1) repeat_h += root->gap;
            repeat_h += child->computed_height;
        } else {
            result->content_indices[result->content_count++] = i;
        }
    }

    // Add gap between repeat section and content
    if (result->repeat_count > 0 && result->content_count > 0) {
        repeat_h += root->gap;
    }
    result->repeat_height = repeat_h;

    // Available height for content per page
    double content_area = page_height - root->padding.top - root->padding.bottom - repeat_h;
    if (content_area < 50) content_area = 50;

    // Walk content children, assign to pages
    double cursor = 0;
    int page_start = 0;

    for (int i = 0; i < result->content_count; i++) {
        int child_idx = result->content_indices[i];
        double child_h = root->children[child_idx]->computed_height;

        // Gap before this child (not before the first on a page)
        double with_gap = (i > page_start) ? root->gap + child_h : child_h;

        if (cursor + with_gap > content_area && i > page_start) {
            // Start a new page
            if (result->page_count >= TSPDF_LAYOUT_MAX_PAGINATED_PAGES) break;
            result->pages[result->page_count].start = page_start;
            result->pages[result->page_count].end = i;
            result->page_count++;
            page_start = i;
            cursor = child_h;
        } else {
            cursor += with_gap;
        }
    }

    // Last page
    if (page_start < result->content_count && result->page_count < TSPDF_LAYOUT_MAX_PAGINATED_PAGES) {
        result->pages[result->page_count].start = page_start;
        result->pages[result->page_count].end = result->content_count;
        result->page_count++;
    }

    if (result->page_count == 0) result->page_count = 1;

    return result->page_count;
}

void tspdf_layout_render_page(TspdfLayout *ctx, TspdfNode *root, const TspdfPaginationResult *result,
                         int page_index, TspdfStream *stream) {
    if (page_index < 0 || page_index >= result->page_count) return;

    double ph = result->page_height;

    // Draw root background for full page
    if (root->style && root->style->has_background) {
        tspdf_stream_set_fill_color(stream, root->style->background);
        tspdf_stream_rect(stream, 0, 0, result->page_width, ph);
        tspdf_stream_fill(stream);
    }

    // Position and render repeating elements at top, filtered by page index
    double cursor_y = root->padding.top;
    bool any_rendered = false;
    for (int i = 0; i < result->repeat_count; i++) {
        TspdfNode *child = root->children[result->repeat_indices[i]];

        // Check if this element should appear on this page
        bool show = false;
        switch (child->repeat_mode) {
            case TSPDF_REPEAT_NONE:      show = false; break;
            case TSPDF_REPEAT_ALL:       show = true; break;
            case TSPDF_REPEAT_FIRST:     show = (page_index == 0); break;
            case TSPDF_REPEAT_NOT_FIRST: show = (page_index != 0); break;
            case TSPDF_REPEAT_EVEN:      show = (page_index % 2 == 0); break;
            case TSPDF_REPEAT_ODD:       show = (page_index % 2 != 0); break;
        }
        if (!show) continue;

        if (any_rendered) cursor_y += root->gap;

        child->computed_x = root->padding.left;
        child->computed_y = cursor_y;
        compute_positions(child);
        render_node(ctx, child, stream, ph);

        cursor_y += child->computed_height;
        any_rendered = true;
    }

    // Gap between repeat section and content
    if (any_rendered) {
        cursor_y += root->gap;
    }

    // Position and render content children for this page
    int start = result->pages[page_index].start;
    int end = result->pages[page_index].end;

    for (int i = start; i < end; i++) {
        int child_idx = result->content_indices[i];
        TspdfNode *child = root->children[child_idx];

        if (i > start) cursor_y += root->gap;

        child->computed_x = root->padding.left;
        child->computed_y = cursor_y;
        compute_positions(child);
        render_node(ctx, child, stream, ph);

        cursor_y += child->computed_height;
    }
}

// --- Option 3: Per-page recompute ---
// Re-runs compute_sizes for each page's content children with actual available height,
// so GROW elements expand to fill the page correctly.

void tspdf_layout_render_page_recompute(TspdfLayout *ctx, TspdfNode *root, const TspdfPaginationResult *result,
                                   int page_index, TspdfStream *stream) {
    if (page_index < 0 || page_index >= result->page_count) return;

    double ph = result->page_height;
    double pw = result->page_width;

    // Draw root background
    if (root->style && root->style->has_background) {
        tspdf_stream_set_fill_color(stream, root->style->background);
        tspdf_stream_rect(stream, 0, 0, pw, ph);
        tspdf_stream_fill(stream);
    }

    // Render repeating elements (same as original)
    double cursor_y = root->padding.top;
    bool any_rendered = false;
    for (int i = 0; i < result->repeat_count; i++) {
        TspdfNode *child = root->children[result->repeat_indices[i]];
        bool show = false;
        switch (child->repeat_mode) {
            case TSPDF_REPEAT_NONE:      show = false; break;
            case TSPDF_REPEAT_ALL:       show = true; break;
            case TSPDF_REPEAT_FIRST:     show = (page_index == 0); break;
            case TSPDF_REPEAT_NOT_FIRST: show = (page_index != 0); break;
            case TSPDF_REPEAT_EVEN:      show = (page_index % 2 == 0); break;
            case TSPDF_REPEAT_ODD:       show = (page_index % 2 != 0); break;
        }
        if (!show) continue;
        if (any_rendered) cursor_y += root->gap;
        child->computed_x = root->padding.left;
        child->computed_y = cursor_y;
        compute_positions(child);
        render_node(ctx, child, stream, ph);
        cursor_y += child->computed_height;
        any_rendered = true;
    }
    if (any_rendered) cursor_y += root->gap;

    // Content children for this page
    int start = result->pages[page_index].start;
    int end = result->pages[page_index].end;
    int count = end - start;
    if (count <= 0) return;

    double content_top = cursor_y;
    double avail_h = ph - content_top - root->padding.bottom;
    double inner_w = pw - root->padding.left - root->padding.right;

    // Compute total fixed height and total grow weight for this page's children
    int num_gaps = count > 1 ? count - 1 : 0;
    double gap_total = num_gaps * root->gap;
    double total_fixed = 0;
    double total_grow_weight = 0;

    for (int i = start; i < end; i++) {
        int child_idx = result->content_indices[i];
        TspdfNode *child = root->children[child_idx];
        if (child->height.mode == TSPDF_SIZE_GROW) {
            total_grow_weight += child->height.value > 0 ? child->height.value : 1;
        } else {
            total_fixed += child->computed_height;
        }
    }

    double remaining = avail_h - total_fixed - gap_total;
    if (remaining < 0) remaining = 0;

    // Re-run compute_sizes for GROW children with their share of remaining space
    for (int i = start; i < end; i++) {
        int child_idx = result->content_indices[i];
        TspdfNode *child = root->children[child_idx];
        if (child->height.mode == TSPDF_SIZE_GROW) {
            double weight = child->height.value > 0 ? child->height.value : 1;
            double share = total_grow_weight > 0 ? remaining * (weight / total_grow_weight) : 0;
            // Re-compute sizes with actual height
            compute_sizes(ctx, child, inner_w, share);
            child->computed_height = share;
        }
    }

    // Position and render
    cursor_y = content_top;
    for (int i = start; i < end; i++) {
        int child_idx = result->content_indices[i];
        TspdfNode *child = root->children[child_idx];
        if (i > start) cursor_y += root->gap;
        child->computed_x = root->padding.left;
        child->computed_y = cursor_y;
        compute_positions(child);
        render_node(ctx, child, stream, ph);
        cursor_y += child->computed_height;
    }
}

// --- Page number rendering ---

void tspdf_layout_render_page_number(TspdfLayout *ctx, TspdfStream *stream, int page_index, int total_pages,
                                const char *font_name, double font_size,
                                TspdfColor color, double page_width, double page_height,
                                TspdfTextAlignment align, double margin_bottom) {
    (void)page_height;
    char buf[64];
    snprintf(buf, sizeof(buf), "Page %d of %d", page_index + 1, total_pages);

    double text_w = measure_text(ctx, font_name, font_size, buf);
    double x;
    switch (align) {
        case TSPDF_TEXT_ALIGN_LEFT:   x = margin_bottom; break;
        case TSPDF_TEXT_ALIGN_CENTER: x = (page_width - text_w) / 2; break;
        case TSPDF_TEXT_ALIGN_RIGHT:  x = page_width - text_w - margin_bottom; break;
        default:                x = (page_width - text_w) / 2; break;
    }
    double y = margin_bottom;

    tspdf_stream_save(stream);
    tspdf_stream_begin_text(stream);
    tspdf_stream_set_font(stream, font_name, font_size);
    tspdf_stream_set_fill_color(stream, color);
    tspdf_stream_text_position(stream, x, y);
    render_show_text(ctx, stream, font_name, buf);
    tspdf_stream_end_text(stream);
    tspdf_stream_restore(stream);
}
