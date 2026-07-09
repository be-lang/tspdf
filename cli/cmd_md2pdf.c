#include "commands.h"
#include "../include/tspdf.h"
#include "../src/util/pdftext.h"
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

/* Every top-level block lands in root's child array, which is capped at
 * TSPDF_LAYOUT_MAX_CHILDREN (layout.h). A document past the cap loses its
 * remaining blocks — warn once instead of dropping them silently. The PDF is
 * still written (exit 0) with the content that fit. */
static int g_blocks_warned = 0;

static void md_add_block(TspdfNode *root, TspdfNode *node) {
    if (tspdf_layout_add_child(root, node) == TSPDF_OK || g_blocks_warned)
        return;
    g_blocks_warned = 1;
    fprintf(stderr, "tspdf md2pdf: warning: document exceeds %d blocks; "
                    "remaining content dropped\n", TSPDF_LAYOUT_MAX_CHILDREN);
}

/* --- Inline Markdown parsing -------------------------------------------- *
 * The layout engine exposes rich-text spans (see src/layout/layout.h), but
 * those render on a single non-wrapping line and are capped at
 * TSPDF_LAYOUT_MAX_SPANS. We therefore only build spans for lines that
 * actually contain inline markup; plain lines keep the wrapping text node so
 * long paragraphs still word-wrap. */

/* A run of text with one inline style, produced by scanning a line. */
typedef enum { SEG_PLAIN, SEG_BOLD, SEG_ITALIC, SEG_MONO, SEG_LINK } SegKind;
typedef struct {
    const char *start;   /* points into the (mutable) source line */
    size_t      len;
    SegKind     kind;
} InlineSeg;

#define MD_MAX_SEGS 64

/* Word character for emphasis flanking rules (cp1252 high bytes count as
 * letters so accented words do not open/close `_` emphasis mid-word). */
static int md_word_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') || u >= 0x80;
}

/* Split `line` into styled segments. `**x**`/`__x__` -> SEG_BOLD,
 * `*x*`/`_x_` -> SEG_ITALIC (with flanking rules: the opener must not be
 * followed by a space, the closer not preceded by one, and `_` only opens or
 * closes at a word boundary so snake_case stays literal), `` `x` `` ->
 * SEG_MONO, `[label](url)` -> SEG_LINK (label text only; the URL is dropped
 * from the visible run), `![alt](url)` -> SEG_ITALIC alt text (inline images
 * are not embedded). Everything else is SEG_PLAIN. Returns the number of
 * segments (capped at MD_MAX_SEGS; on overflow the tail is folded into the
 * last plain segment so no text is lost). */
static int split_inline_segments(const char *line, InlineSeg *segs, int max_segs) {
    int n = 0;
    const char *p = line;
    const char *plain_start = p;

    while (*p) {
        const char *seg = NULL;   /* inner text start */
        size_t seg_len = 0;
        const char *resume = NULL;
        SegKind kind = SEG_PLAIN;

        if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
            char d = p[0];
            const char *close = NULL;
            for (const char *q = p + 2; *q; q++) {
                if (q[0] == d && q[1] == d) { close = q; break; }
            }
            if (close && close > p + 2) {
                seg = p + 2; seg_len = (size_t)(close - seg);
                resume = close + 2; kind = SEG_BOLD;
            }
        } else if ((p[0] == '*' && p[1] != '*') ||
                   (p[0] == '_' && p[1] != '_' &&
                    (p == line || !md_word_char(p[-1])))) {
            char d = p[0];
            if (p[1] != ' ' && p[1] != '\0') {
                const char *close = NULL;
                for (const char *q = p + 2; *q; q++) {
                    if (q[0] == d && q[-1] != ' ' &&
                        (d == '*' || !md_word_char(q[1]))) { close = q; break; }
                }
                if (close) {
                    seg = p + 1; seg_len = (size_t)(close - seg);
                    resume = close + 1; kind = SEG_ITALIC;
                }
            }
        } else if (p[0] == '!' && p[1] == '[') {
            const char *rb = strchr(p + 1, ']');
            if (rb && rb[1] == '(') {
                const char *rp = strchr(rb + 2, ')');
                if (rp) {
                    seg = p + 2; seg_len = (size_t)(rb - seg);  /* alt only */
                    resume = rp + 1; kind = SEG_ITALIC;
                }
            }
        } else if (p[0] == '`') {
            const char *close = strchr(p + 1, '`');
            if (close && close > p + 1) {
                seg = p + 1; seg_len = (size_t)(close - seg);
                resume = close + 1; kind = SEG_MONO;
            }
        } else if (p[0] == '[') {
            const char *rb = strchr(p, ']');
            if (rb && rb[1] == '(') {
                const char *rp = strchr(rb + 2, ')');
                if (rp) {
                    seg = p + 1; seg_len = (size_t)(rb - seg);  /* label only */
                    resume = rp + 1; kind = SEG_LINK;
                }
            }
        }

        if (seg) {
            /* Emit pending plain run, then the styled segment. */
            if (p > plain_start && n < max_segs) {
                segs[n].start = plain_start;
                segs[n].len = (size_t)(p - plain_start);
                segs[n].kind = SEG_PLAIN;
                n++;
            }
            if (n < max_segs) {
                segs[n].start = seg;
                segs[n].len = seg_len;
                segs[n].kind = kind;
                n++;
            }
            p = resume;
            plain_start = p;
        } else {
            p++;
        }
    }

    if (p > plain_start && n < max_segs) {
        segs[n].start = plain_start;
        segs[n].len = (size_t)(p - plain_start);
        segs[n].kind = SEG_PLAIN;
        n++;
    }
    return n;
}

/* True if the line contains any inline construct the segment scanner would
 * style (exact: runs the scanner, so flanking rules match the renderer). */
static int line_has_inline(const char *s) {
    InlineSeg segs[MD_MAX_SEGS];
    int n = split_inline_segments(s, segs, MD_MAX_SEGS);
    if (n > 1) return 1;
    return n == 1 && segs[0].kind != SEG_PLAIN;
}

/* Render `line` with inline styling onto `node`.
 * fonts: regular / bold / italic / mono.
 *
 * Rich-text spans (src/layout/layout.h) render on a single non-wrapping line
 * and are capped at TSPDF_LAYOUT_MAX_SPANS, so we only use them when the
 * styled line fits within avail_width and the span cap. Otherwise we fall
 * back to plain (still word-wrapping) text built from the segments with the
 * markup stripped — styling is lost but every character is preserved and no
 * literal `*`/backtick/`](` markers leak into the PDF. Either way
 * `node->text.text` was pre-seeded with the raw line by the caller; when
 * spans are added they override it, and on fallback we overwrite it. */
static void parse_inline_spans(TspdfLayout *ctx, TspdfNode *node, const char *line,
                               const char *regular, const char *bold,
                               const char *italic, const char *mono,
                               double size, TspdfColor color, double avail_width) {
    InlineSeg segs[MD_MAX_SEGS];
    int n = split_inline_segments(line, segs, MD_MAX_SEGS);
    if (n == 0) return;

    /* Arena copies of every segment; also reused by the plain fallback. */
    char *texts[MD_MAX_SEGS];
    const char *fonts[MD_MAX_SEGS];
    for (int i = 0; i < n; i++) {
        texts[i] = (char *)tspdf_arena_alloc(ctx->arena, segs[i].len + 1);
        if (!texts[i]) return;  /* keep node's raw text (still no crash) */
        memcpy(texts[i], segs[i].start, segs[i].len);
        texts[i][segs[i].len] = '\0';
        switch (segs[i].kind) {
            case SEG_BOLD:   fonts[i] = bold;    break;
            case SEG_ITALIC: fonts[i] = italic;  break;
            case SEG_MONO:   fonts[i] = mono;    break;
            default:         fonts[i] = regular; break;
        }
    }

    if (n <= TSPDF_LAYOUT_MAX_SPANS) {
        double total = 0;
        for (int i = 0; i < n; i++) {
            if (ctx->measure_text)
                total += ctx->measure_text(fonts[i], size, texts[i], ctx->measure_userdata);
        }
        if (total <= avail_width) {
            for (int i = 0; i < n; i++) {
                if (segs[i].len == 0) continue;
                int decoration = segs[i].kind == SEG_LINK ? TSPDF_TEXT_DECOR_UNDERLINE : 0;
                if (!tspdf_layout_text_add_span(node, texts[i], fonts[i], size, color, decoration)) {
                    node->text.span_count = 0;  /* should not happen: n <= cap */
                    goto fallback;
                }
            }
            return;
        }
    }

fallback:
    {
        /* Concatenate de-marked segment text into one wrapping plain string. */
        size_t total = 0;
        for (int i = 0; i < n; i++) total += segs[i].len;
        char *buf = (char *)tspdf_arena_alloc(ctx->arena, total + 1);
        if (!buf) return;  /* keep node's raw text (still no crash) */
        size_t off = 0;
        for (int i = 0; i < n; i++) {
            memcpy(buf + off, texts[i], segs[i].len);
            off += segs[i].len;
        }
        buf[off] = '\0';
        node->text.text = buf;
        node->text.span_count = 0;
    }
}

/* Emit a paragraph node (also used to flush a stashed pipe-table candidate
 * that turned out not to be a table header). */
static void md_add_paragraph(TspdfLayout *ctx, TspdfNode *root, const char *text,
                             const char *sans, const char *bold, const char *italic,
                             const char *mono, double avail_width) {
    TspdfColor para_color = tspdf_color_from_u8(40, 50, 80);
    TspdfNode *node = tspdf_layout_text(ctx, text, sans, 11);
    node->text.wrap = TSPDF_WRAP_WORD;
    node->text.color = para_color;
    node->width = tspdf_size_grow();
    if (line_has_inline(text)) {
        parse_inline_spans(ctx, node, text, sans, bold, italic, mono,
                           11, para_color, avail_width);
    }
    md_add_block(root, node);
}

/* --- Pipe tables --------------------------------------------------------- *
 * GitHub-style: a header row, a |---|:---:| separator (colons set column
 * alignment), then body rows. Rows must start with '|'; "\|" inside a cell
 * is a literal pipe. Inline markup in cells is stripped to plain text. */

#define MD_TABLE_MAX_COLS TSPDF_LAYOUT_MAX_CHILDREN

static int md_is_table_row(const char *line) {
    return line[0] == '|';
}

/* Concatenate the de-marked segment texts: cell content renders plain. */
static char *md_strip_inline_markers(TspdfLayout *ctx, const char *s) {
    InlineSeg segs[MD_MAX_SEGS];
    int n = split_inline_segments(s, segs, MD_MAX_SEGS);
    size_t total = 0;
    for (int i = 0; i < n; i++) total += segs[i].len;
    char *buf = (char *)tspdf_arena_alloc(ctx->arena, total + 1);
    if (!buf) return NULL;
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        memcpy(buf + off, segs[i].start, segs[i].len);
        off += segs[i].len;
    }
    buf[off] = '\0';
    return buf;
}

/* Split a table row into trimmed arena cell strings. The leading '|' and a
 * trailing '|' delimit cells; interior empty cells are kept. Returns the
 * cell count, or -1 on allocation failure. */
static int md_parse_table_cells(TspdfLayout *ctx, const char *line,
                                const char **cells, int max_cells) {
    const char *p = line;
    if (*p == '|') p++;
    int n = 0;
    while (n < max_cells) {
        /* Cell extends to the next unescaped '|' or end of line. */
        const char *q = p;
        while (*q && !(*q == '|' && (q == p || q[-1] != '\\'))) q++;

        /* Copy, unescaping "\|" and trimming surrounding spaces. */
        size_t raw_len = (size_t)(q - p);
        char *buf = (char *)tspdf_arena_alloc(ctx->arena, raw_len + 1);
        if (!buf) return -1;
        size_t out = 0;
        for (size_t i = 0; i < raw_len; i++) {
            if (p[i] == '\\' && i + 1 < raw_len && p[i + 1] == '|') {
                buf[out++] = '|';
                i++;
            } else {
                buf[out++] = p[i];
            }
        }
        while (out > 0 && buf[out - 1] == ' ') out--;
        buf[out] = '\0';
        char *start = buf;
        while (*start == ' ') start++;

        cells[n] = md_strip_inline_markers(ctx, start);
        if (!cells[n]) return -1;
        n++;

        if (*q == '\0') break;
        p = q + 1;
        if (*p == '\0') break;  /* trailing '|': no empty tail cell */
    }
    return n;
}

/* True if `line` is a separator row: every cell matches ^:?-+:?$ (spaces
 * allowed around it). Writes per-column alignment and the column count. */
static int md_is_table_separator(const char *line, TspdfTextAlignment *aligns,
                                 int max_cols, int *ncols_out) {
    if (line[0] != '|') return 0;
    const char *p = line + 1;
    int n = 0;
    while (*p) {
        while (*p == ' ') p++;
        int left = 0, right = 0, dashes = 0;
        if (*p == ':') { left = 1; p++; }
        while (*p == '-') { dashes++; p++; }
        if (*p == ':') { right = 1; p++; }
        while (*p == ' ') p++;
        if (dashes == 0) return 0;
        if (n < max_cols) {
            aligns[n] = (left && right) ? TSPDF_TEXT_ALIGN_CENTER
                      : right           ? TSPDF_TEXT_ALIGN_RIGHT
                                        : TSPDF_TEXT_ALIGN_LEFT;
        }
        n++;
        if (*p == '|') { p++; continue; }
        if (*p == '\0') break;
        return 0;
    }
    if (n == 0 || n > max_cols) return 0;
    *ncols_out = n;
    return 1;
}

/* Build the collected table and add it to the layout tree. */
static void md_emit_table(TspdfLayout *ctx, TspdfNode *root,
                          const char **headers, int ncols,
                          const TspdfTextAlignment *aligns,
                          const char **rows_flat, int nrows,
                          const char *sans) {
    TspdfTableStyle style = {
        .font_name = sans,
        .font_size = 10,
        .text_color = tspdf_color_from_u8(40, 50, 80),
        .header_bg = tspdf_color_from_u8(10, 15, 50),
        .header_text_color = tspdf_color_rgb(1, 1, 1),
        .row_bg_even = tspdf_color_from_u8(245, 246, 252),
        .row_bg_odd = tspdf_color_rgb(1, 1, 1),
        .border_color = tspdf_color_from_u8(200, 205, 220),
        .border_width = 1,
        .row_height = 0,  /* rows fit their content, cells word-wrap */
        .header_height = 24,
        .padding = 6,
    };

    /* One column-width pass over all rows so every chunk lines up. */
    double col_widths[MD_TABLE_MAX_COLS];
    tspdf_layout_table_compute_widths(ctx, headers, rows_flat, nrows, ncols,
                                      style, col_widths);

    /* A table node cannot split across pages, so long tables are emitted as
     * stacked chunks (header repeated); pagination breaks between chunks. */
    const int rows_per_chunk = 28;
    int r0 = 0;
    do {
        int chunk = nrows - r0 < rows_per_chunk ? nrows - r0 : rows_per_chunk;
        TspdfNode *tbl = tspdf_layout_table(ctx, headers, col_widths, ncols, style);
        if (!tbl) return;
        for (int r = 0; r < chunk; r++)
            tspdf_layout_table_add_row(ctx, tbl, &rows_flat[(r0 + r) * ncols],
                                       col_widths, ncols, style);

        /* Column alignment from the separator row. children[0] is the header
         * row (kept centered); each data-row cell box holds one text node. */
        for (int r = 1; r < tbl->child_count; r++) {
            TspdfNode *row = tbl->children[r];
            for (int c = 0; c < row->child_count && c < ncols; c++) {
                TspdfNode *cell = row->children[c];
                if (cell->child_count > 0 && cell->children[0]->type == TSPDF_NODE_TEXT)
                    cell->children[0]->text.alignment = aligns[c];
            }
        }
        md_add_block(root, tbl);
        r0 += chunk;
    } while (r0 < nrows);
}

/* --- Block-level images --------------------------------------------------- */

static int md_path_is_png(const char *path, size_t len) {
    if (len < 4) return 0;
    const char *ext = path + len - 4;
    return ext[0] == '.' &&
           (ext[1] == 'p' || ext[1] == 'P') &&
           (ext[2] == 'n' || ext[2] == 'N') &&
           (ext[3] == 'g' || ext[3] == 'G');
}

/* A whole line of the form ![alt](path): embed the image scaled to fit the
 * content box (aspect preserved, centered, never upscaled). `raw` must be the
 * raw UTF-8 line — paths keep their original bytes for fopen(); only the alt
 * text is re-encoded for rendering. A missing/unloadable image warns once on
 * stderr and renders the alt text in italics instead. Returns 1 if the line
 * was a block image (even when the fallback was used), 0 otherwise. */
static int md_try_image(TspdfLayout *ctx, TspdfNode *root, TspdfWriter *doc,
                        const char *raw, const char *md_path,
                        const char *italic_font, double content_w,
                        double content_h, int lineno) {
    if (raw[0] != '!' || raw[1] != '[') return 0;
    const char *alt_start = raw + 2;
    const char *rb = strchr(alt_start, ']');
    if (!rb || rb[1] != '(') return 0;
    const char *path_start = rb + 2;
    const char *rp = strchr(path_start, ')');
    if (!rp) return 0;
    for (const char *t = rp + 1; *t; t++)
        if (*t != ' ' && *t != '\t') return 0;  /* trailing junk: not block image */

    size_t alt_len = (size_t)(rb - alt_start);
    while (*path_start == ' ') path_start++;
    size_t path_len = (size_t)(rp - path_start);
    /* Cut an optional "title" part: ![alt](path "title") */
    for (size_t i = 0; i < path_len; i++) {
        if (path_start[i] == ' ') { path_len = i; break; }
    }

    /* Resolve relative to the directory of the .md file. */
    size_t dir_len = 0;
    if (path_len == 0 || path_start[0] != '/') {
        const char *slash = strrchr(md_path, '/');
        if (slash) dir_len = (size_t)(slash - md_path) + 1;
    }
    char *full = (char *)tspdf_arena_alloc(ctx->arena, dir_len + path_len + 1);
    if (!full) return 0;
    memcpy(full, md_path, dir_len);
    memcpy(full + dir_len, path_start, path_len);
    full[dir_len + path_len] = '\0';

    const char *img_name = NULL;
    if (path_len > 0) {
        img_name = md_path_is_png(full, dir_len + path_len)
                       ? tspdf_writer_add_png_image(doc, full)
                       : tspdf_writer_add_jpeg_image(doc, full);
    }

    if (!img_name) {
        fprintf(stderr, "tspdf md2pdf: warning: line %d: cannot load image '%s'; "
                        "showing alt text\n", lineno, full);
        char *alt = (char *)tspdf_arena_alloc(ctx->arena, alt_len + 1);
        if (!alt) return 1;
        memcpy(alt, alt_start, alt_len);
        alt[alt_len] = '\0';
        tspdf_utf8_to_cp1252_lossy(alt, alt, NULL);
        TspdfNode *node = tspdf_layout_text(ctx, alt[0] ? alt : "[image]",
                                            italic_font, 11);
        node->text.color = tspdf_color_from_u8(110, 115, 130);
        node->text.wrap = TSPDF_WRAP_WORD;
        node->width = tspdf_size_grow();
        md_add_block(root, node);
        return 1;
    }

    /* Scale to fit the content width (and one page height), keep aspect. */
    const TspdfImage *img = &doc->images[doc->image_count - 1];
    double dw = (double)img->width, dh = (double)img->height;
    if (dw <= 0 || dh <= 0) { dw = 1; dh = 1; }
    if (dw > content_w) { dh *= content_w / dw; dw = content_w; }
    if (dh > content_h) { dw *= content_h / dh; dh = content_h; }

    TspdfNode *wrap = tspdf_layout_box(ctx);
    wrap->width = tspdf_size_grow();
    wrap->direction = TSPDF_DIR_ROW;
    wrap->align_x = TSPDF_ALIGN_CENTER;

    TspdfNode *imgbox = tspdf_layout_box(ctx);
    imgbox->width = tspdf_size_fixed(dw);
    imgbox->height = tspdf_size_fixed(dh);
    TspdfBoxStyle *st = tspdf_layout_node_style(ctx, imgbox);
    st->bg_image = img_name;  /* writer-owned name, outlives rendering */
    tspdf_layout_add_child(wrap, imgbox);
    md_add_block(root, wrap);
    return 1;
}

/* Detect a leading ordered-list marker "N." or "N)" followed by a space.
 * Returns the parsed number (>=0) and sets *content to the text after the
 * marker+space; returns -1 if the line is not an ordered-list item. */
static long ordered_list_marker(const char *line, const char **content) {
    if (line[0] < '0' || line[0] > '9') return -1;
    const char *p = line;
    long n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > 99999999L) return -1;  /* implausibly long; treat as paragraph */
        p++;
    }
    if ((*p == '.' || *p == ')') && p[1] == ' ') {
        *content = p + 2;
        return n;
    }
    return -1;
}

int cmd_md2pdf(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf md2pdf <input.md> -o <output.pdf>\n");
        printf("\nConvert a Markdown document to a styled PDF.\n");
        printf("Supports headings, lists, code blocks, blockquotes, rules, pipe tables,\n");
        printf("and block-level ![alt](path) images (JPEG/PNG, path relative to the .md).\n");
        printf("Text uses the built-in WinAnsi (cp1252) fonts; characters outside that\n");
        printf("range (emoji, CJK, ...) are replaced with '?' and warned about.\n");
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
    size_t nread = fread(md, 1, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) {
        free(md);
        fprintf(stderr, "tspdf md2pdf: failed to read input file\n");
        return 1;
    }
    md[nread] = '\0';

    TspdfWriter *doc = tspdf_writer_create();
    if (!doc) { free(md); fprintf(stderr, "tspdf md2pdf: out of memory\n"); return 1; }
    g_doc = doc;

    tspdf_writer_set_title(doc, "Document");
    tspdf_writer_set_creator(doc, "tspdf");

    const char *sans = tspdf_writer_add_builtin_font(doc, "Helvetica");
    const char *bold = tspdf_writer_add_builtin_font(doc, "Helvetica-Bold");
    const char *italic = tspdf_writer_add_builtin_font(doc, "Helvetica-Oblique");
    const char *bolditalic = tspdf_writer_add_builtin_font(doc, "Helvetica-BoldOblique");
    const char *mono = tspdf_writer_add_builtin_font(doc, "Courier");

    TspdfArena arena = tspdf_arena_create(8 * 1024 * 1024);
    TspdfLayout ctx = tspdf_layout_create(&arena);
    ctx.measure_text = measure_cb;
    ctx.font_line_height = lh_cb;
    ctx.doc = doc;

    double W = TSPDF_PAGE_A4_WIDTH, H = TSPDF_PAGE_A4_HEIGHT;
    /* Width available to content inside the root padding; inline spans only
     * render single-line, so parse_inline_spans falls back to wrapped plain
     * text when the styled line would exceed this. */
    double content_w = W - 2 * 52;

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
    int lineno = 0;

    /* Pipe-table collector: a '|' line is stashed until the next line shows
     * whether it is a table header (separator row follows) or just text. */
    const char *tbl_header_line = NULL;   /* arena copy of the stashed line */
    int tbl_active = 0;
    const char *tbl_headers[MD_TABLE_MAX_COLS];
    TspdfTextAlignment tbl_aligns[MD_TABLE_MAX_COLS];
    int tbl_cols = 0;
    const char **tbl_rows = NULL;         /* flat malloc'd cells, row-major */
    int tbl_nrows = 0;
    int tbl_rows_cap = 0;                 /* capacity in cells */

    char *line = md;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);
        lineno++;

        /* Temporary null-terminate the line */
        char saved = line[line_len];
        line[line_len] = '\0';

        /* Strip trailing \r */
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        /* Preserve the raw UTF-8 bytes of a potential block-image line: the
         * cp1252 re-encode below would corrupt non-ASCII image paths, which
         * must keep their original bytes for fopen(). */
        const char *raw_image_line = NULL;
        if (!in_code_block && line[0] == '!' && line[1] == '[') {
            size_t raw_len = strlen(line);
            char *copy = (char *)tspdf_arena_alloc(ctx.arena, raw_len + 1);
            if (copy) {
                memcpy(copy, line, raw_len + 1);
                raw_image_line = copy;
            }
        }

        /* The built-in (base14) fonts draw text as WinAnsiEncoding (cp1252),
         * so re-encode the UTF-8 line in place (cp1252 output is never longer
         * than the input). Characters outside cp1252 become '?' with a
         * warning instead of aborting the whole document; see --help. */
        uint32_t bad_cp = 0;
        if (tspdf_utf8_to_cp1252_lossy(line, line, &bad_cp) > 0) {
            char ch[5];
            ch[tspdf_utf8_encode(bad_cp, ch)] = '\0';
            fprintf(stderr, "tspdf md2pdf: warning: line %d: character '%s' (U+%04X) is "
                            "outside WinAnsi (cp1252); substituted '?'\n",
                    lineno, ch, (unsigned)bad_cp);
        }
        /* Length of the re-encoded line (line_len stays the raw length so the
         * saved byte is restored at the right offset below). */
        size_t text_len = strlen(line);

        /* Detect ordered-list marker ("1." / "1)") up front so the if-chain
         * below can branch on it. Only meaningful outside code blocks. */
        const char *ord_content = NULL;
        long ord_number = -1;
        if (!in_code_block) {
            ord_number = ordered_list_marker(line, &ord_content);
            if (ord_number < 0) ord_content = NULL;
        }

        /* Pipe-table collector. Runs before the block chain: it may consume
         * the line (header stash / separator / body row) or flush the table
         * and let the current line fall through to normal handling. */
        int consumed = 0;
        if (!in_code_block) {
            if (tbl_active) {
                if (md_is_table_row(line)) {
                    const char *cells[MD_TABLE_MAX_COLS];
                    int ncells = md_parse_table_cells(&ctx, line, cells, MD_TABLE_MAX_COLS);
                    if (ncells >= 0) {
                        if ((tbl_nrows + 1) * tbl_cols > tbl_rows_cap) {
                            int new_cap = tbl_rows_cap ? tbl_rows_cap * 2 : 16 * tbl_cols;
                            const char **grown = realloc(tbl_rows,
                                (size_t)new_cap * sizeof(*tbl_rows));
                            if (grown) { tbl_rows = grown; tbl_rows_cap = new_cap; }
                        }
                        if ((tbl_nrows + 1) * tbl_cols <= tbl_rows_cap) {
                            /* Pad short rows with ""; drop extra cells. */
                            for (int c = 0; c < tbl_cols; c++)
                                tbl_rows[tbl_nrows * tbl_cols + c] = c < ncells ? cells[c] : "";
                            tbl_nrows++;
                        }
                    }
                    consumed = 1;
                } else {
                    md_emit_table(&ctx, root, tbl_headers, tbl_cols, tbl_aligns,
                                  tbl_rows, tbl_nrows, sans);
                    free(tbl_rows);
                    tbl_rows = NULL;
                    tbl_nrows = tbl_rows_cap = 0;
                    tbl_active = 0;
                    /* current line falls through to normal handling */
                }
            } else if (tbl_header_line) {
                const char *hdr = tbl_header_line;
                tbl_header_line = NULL;
                if (md_is_table_separator(line, tbl_aligns, MD_TABLE_MAX_COLS, &tbl_cols)) {
                    int n = md_parse_table_cells(&ctx, hdr, tbl_headers, MD_TABLE_MAX_COLS);
                    if (n > 0) {
                        /* Header defines the column count; pad alignment for
                         * columns the separator did not declare. */
                        for (int c = tbl_cols; c < n; c++)
                            tbl_aligns[c] = TSPDF_TEXT_ALIGN_LEFT;
                        tbl_cols = n;
                        tbl_active = 1;
                        consumed = 1;
                    }
                }
                if (!consumed) {
                    /* Not a table after all: flush the stashed line as a
                     * paragraph; the current line falls through. */
                    md_add_paragraph(&ctx, root, hdr, sans, bold, italic, mono, content_w);
                }
            } else if (md_is_table_row(line)) {
                char *copy = (char *)tspdf_arena_alloc(ctx.arena, text_len + 1);
                if (copy) {
                    memcpy(copy, line, text_len + 1);
                    tbl_header_line = copy;
                    consumed = 1;
                }
            }
        }

        if (consumed) {
            /* line was taken by the table collector */
        } else if (!in_code_block && strncmp(line, "```", 3) == 0) {
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
            md_add_block(root, code_box);
        } else if (in_code_block) {
            /* Accumulate code lines */
            if (code_len > 0 && code_len < (int)sizeof(code_buf) - 2) {
                code_buf[code_len++] = '\n';
            }
            int remaining = (int)sizeof(code_buf) - code_len - 1;
            if (remaining > 0) {
                int copy = (int)text_len < remaining ? (int)text_len : remaining;
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

            TspdfColor heading_color = tspdf_color_from_u8(10, 15, 50);
            TspdfNode *node = tspdf_layout_text(&ctx, text, bold, sz);
            node->text.color = heading_color;
            node->width = tspdf_size_grow();
            if (line_has_inline(text)) {
                /* Headings default to bold; inline `code`/links still styled. */
                parse_inline_spans(&ctx, node, text, bold, bold, bolditalic, mono,
                                   sz, heading_color, content_w);
            }
            md_add_block(root, node);
        } else if ((line[0] == '-' || line[0] == '*') && line[1] == ' ') {
            /* List item */
            TspdfNode *row = tspdf_layout_box(&ctx);
            row->direction = TSPDF_DIR_ROW;
            row->width = tspdf_size_grow();
            row->gap = 8;

            TspdfNode *bullet = tspdf_layout_text(&ctx, "\x95", sans, 11);  /* bullet (U+2022) in cp1252 */
            bullet->text.color = tspdf_color_from_u8(79, 110, 247);
            tspdf_layout_add_child(row, bullet);

            TspdfColor item_color = tspdf_color_from_u8(40, 50, 80);
            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = item_color;
            if (line_has_inline(line + 2)) {
                parse_inline_spans(&ctx, txt, line + 2, sans, bold, italic, mono,
                                   11, item_color, content_w - 20);
            }
            tspdf_layout_add_child(row, txt);

            md_add_block(root, row);
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

            TspdfColor quote_color = tspdf_color_from_u8(80, 90, 110);
            TspdfNode *txt = tspdf_layout_text(&ctx, line + 2, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = quote_color;
            if (line_has_inline(line + 2)) {
                parse_inline_spans(&ctx, txt, line + 2, sans, bold, italic, mono,
                                   11, quote_color, content_w - 16);
            }
            tspdf_layout_add_child(bq, txt);

            md_add_block(root, bq);
        } else if (ord_content != NULL) {
            /* Ordered list item: render "N." marker + inline-styled text. */
            TspdfNode *row = tspdf_layout_box(&ctx);
            row->direction = TSPDF_DIR_ROW;
            row->width = tspdf_size_grow();
            row->gap = 8;

            char marker[24];
            snprintf(marker, sizeof(marker), "%ld.", ord_number);
            TspdfNode *num = tspdf_layout_text(&ctx, marker, bold, 11);
            num->text.color = tspdf_color_from_u8(79, 110, 247);
            tspdf_layout_add_child(row, num);

            TspdfColor item_color = tspdf_color_from_u8(40, 50, 80);
            TspdfNode *txt = tspdf_layout_text(&ctx, ord_content, sans, 11);
            txt->text.wrap = TSPDF_WRAP_WORD;
            txt->width = tspdf_size_grow();
            txt->text.color = item_color;
            if (line_has_inline(ord_content)) {
                parse_inline_spans(&ctx, txt, ord_content, sans, bold, italic, mono,
                                   11, item_color, content_w - 30);
            }
            tspdf_layout_add_child(row, txt);

            md_add_block(root, row);
        } else if (raw_image_line &&
                   md_try_image(&ctx, root, doc, raw_image_line, input, italic,
                                content_w, H - 2 * 52, lineno)) {
            /* Block image (or its alt-text fallback) was emitted. */
        } else if ((strncmp(line, "---", 3) == 0 || strncmp(line, "***", 3) == 0) && text_len <= 5) {
            /* Horizontal rule — render as a thin box */
            TspdfNode *hr = tspdf_layout_box(&ctx);
            hr->width = tspdf_size_grow();
            hr->height = tspdf_size_fixed(1);
            TspdfBoxStyle *hs = tspdf_layout_node_style(&ctx, hr);
            hs->has_background = true;
            hs->background = tspdf_color_from_u8(200, 205, 220);
            md_add_block(root, hr);
        } else if (text_len > 0) {
            /* Paragraph */
            TspdfColor para_color = tspdf_color_from_u8(40, 50, 80);
            TspdfNode *node = tspdf_layout_text(&ctx, line, sans, 11);
            node->text.wrap = TSPDF_WRAP_WORD;
            node->text.color = para_color;
            node->width = tspdf_size_grow();
            if (line_has_inline(line)) {
                parse_inline_spans(&ctx, node, line, sans, bold, italic, mono,
                                   11, para_color, content_w);
            }
            md_add_block(root, node);
        }
        /* Skip blank lines (no output) */

        line[line_len] = saved;
        line = eol ? eol + 1 : NULL;
    }

    /* Flush table state left open at end of input. */
    if (tbl_header_line) {
        md_add_paragraph(&ctx, root, tbl_header_line, sans, bold, italic, mono, content_w);
    }
    if (tbl_active) {
        md_emit_table(&ctx, root, tbl_headers, tbl_cols, tbl_aligns,
                      tbl_rows, tbl_nrows, sans);
    }
    free(tbl_rows);

    free(md);

    TspdfPaginationResult pagination;
    int num_pages = tspdf_layout_compute_paginated(&ctx, root, W, H, &pagination);
    for (int pg = 0; pg < num_pages; pg++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        tspdf_layout_render_page_recompute(&ctx, root, &pagination, pg, page);
    }

    TspdfError err = tspdf_writer_save(doc, output);
    tspdf_writer_destroy(doc);
    /* Free realloc'd layout arrays (children/spans/wrapped lines) before
     * destroying the arena, per the layout API contract (layout.h). */
    tspdf_layout_tree_free(root);
    tspdf_arena_destroy(&arena);

    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf md2pdf: failed to save '%s': %s\n", output, tspdf_error_string(err));
        return 1;
    }

    printf("Markdown (%d page%s) -> %s\n", num_pages, num_pages == 1 ? "" : "s", output);
    return 0;
}
