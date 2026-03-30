#include "pdf_stream.h"
#include "tspdf_writer.h"
#include <string.h>
#include <math.h>

// Kappa for approximating quarter-circle with cubic bezier
// kappa = 4 * (sqrt(2) - 1) / 3
#define KAPPA 0.5522847498

TspdfStream tspdf_stream_create(void) {
    TspdfStream s = {0};
    s.buf = tspdf_buffer_create(4096);
    return s;
}

void tspdf_stream_destroy(TspdfStream *s) {
    tspdf_buffer_destroy(&s->buf);
}

void tspdf_stream_save(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "q\n");
}

void tspdf_stream_restore(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "Q\n");
}

void tspdf_stream_set_fill_color(TspdfStream *s, TspdfColor c) {
    tspdf_buffer_append_double(&s->buf, c.r, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, c.g, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, c.b, 4);
    tspdf_buffer_append_str(&s->buf, " rg\n");
}

void tspdf_stream_set_stroke_color(TspdfStream *s, TspdfColor c) {
    tspdf_buffer_append_double(&s->buf, c.r, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, c.g, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, c.b, 4);
    tspdf_buffer_append_str(&s->buf, " RG\n");
}

void tspdf_stream_set_line_width(TspdfStream *s, double w) {
    tspdf_buffer_append_double(&s->buf, w, 4);
    tspdf_buffer_append_str(&s->buf, " w\n");
}

void tspdf_stream_set_dash(TspdfStream *s, const double *pattern, int count, double phase) {
    tspdf_buffer_append_str(&s->buf, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) tspdf_buffer_append_str(&s->buf, " ");
        tspdf_buffer_printf(&s->buf, "%.2f", pattern[i]);
    }
    tspdf_buffer_printf(&s->buf, "] %.2f d\n", phase);
}

void tspdf_stream_move_to(TspdfStream *s, double x, double y) {
    tspdf_buffer_append_double(&s->buf, x, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y, 4);
    tspdf_buffer_append_str(&s->buf, " m\n");
}

void tspdf_stream_line_to(TspdfStream *s, double x, double y) {
    tspdf_buffer_append_double(&s->buf, x, 4);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y, 4);
    tspdf_buffer_append_str(&s->buf, " l\n");
}

void tspdf_stream_curve_to(TspdfStream *s, double x1, double y1, double x2, double y2, double x3, double y3) {
    tspdf_buffer_append_double(&s->buf, x1, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y1, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, x2, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y2, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, x3, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y3, 4);
    tspdf_buffer_append_str(&s->buf, " c\n");
}

void tspdf_stream_rect(TspdfStream *s, double x, double y, double w, double h) {
    tspdf_buffer_append_double(&s->buf, x, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, w, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, h, 4);
    tspdf_buffer_append_str(&s->buf, " re\n");
}

void tspdf_stream_close_path(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "h\n");
}

void tspdf_stream_fill(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "f\n");
}

void tspdf_stream_stroke(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "S\n");
}

void tspdf_stream_fill_stroke(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "B\n");
}

void tspdf_stream_clip(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "W n\n");
}

void tspdf_stream_rounded_rect(TspdfStream *s, double x, double y, double w, double h,
                             double tl, double tr, double br, double bl) {
    // PDF coordinate system: y increases upward, but we treat y as top-left origin
    // Build path clockwise from top-left corner

    // Top-left corner
    tspdf_stream_move_to(s, x + tl, y);

    // Top edge → top-right corner
    tspdf_stream_line_to(s, x + w - tr, y);
    if (tr > 0) {
        tspdf_stream_curve_to(s, x + w - tr + tr * KAPPA, y,
                               x + w, y + tr - tr * KAPPA,
                               x + w, y + tr);
    }

    // Right edge → bottom-right corner
    tspdf_stream_line_to(s, x + w, y + h - br);
    if (br > 0) {
        tspdf_stream_curve_to(s, x + w, y + h - br + br * KAPPA,
                               x + w - br + br * KAPPA, y + h,
                               x + w - br, y + h);
    }

    // Bottom edge → bottom-left corner
    tspdf_stream_line_to(s, x + bl, y + h);
    if (bl > 0) {
        tspdf_stream_curve_to(s, x + bl - bl * KAPPA, y + h,
                               x, y + h - bl + bl * KAPPA,
                               x, y + h - bl);
    }

    // Left edge → top-left corner
    tspdf_stream_line_to(s, x, y + tl);
    if (tl > 0) {
        tspdf_stream_curve_to(s, x, y + tl - tl * KAPPA,
                               x + tl - tl * KAPPA, y,
                               x + tl, y);
    }

    tspdf_stream_close_path(s);
}

void tspdf_stream_rounded_rect_pdf(TspdfStream *s, double x, double y, double w, double h,
                                 double tl, double tr, double br, double bl) {
    // PDF coordinates: (x,y) is bottom-left, y increases upward
    // Visual corners: tl=top-left, tr=top-right, br=bottom-right, bl=bottom-left
    // Top of rect is at y+h, bottom at y

    // Start at bottom-left corner (just above the bl radius)
    tspdf_stream_move_to(s, x, y + bl);

    // Left edge going up → top-left corner
    tspdf_stream_line_to(s, x, y + h - tl);
    if (tl > 0) {
        tspdf_stream_curve_to(s, x, y + h - tl + tl * KAPPA,
                               x + tl - tl * KAPPA, y + h,
                               x + tl, y + h);
    }

    // Top edge going right → top-right corner
    tspdf_stream_line_to(s, x + w - tr, y + h);
    if (tr > 0) {
        tspdf_stream_curve_to(s, x + w - tr + tr * KAPPA, y + h,
                               x + w, y + h - tr + tr * KAPPA,
                               x + w, y + h - tr);
    }

    // Right edge going down → bottom-right corner
    tspdf_stream_line_to(s, x + w, y + br);
    if (br > 0) {
        tspdf_stream_curve_to(s, x + w, y + br - br * KAPPA,
                               x + w - br + br * KAPPA, y,
                               x + w - br, y);
    }

    // Bottom edge going left → bottom-left corner
    tspdf_stream_line_to(s, x + bl, y);
    if (bl > 0) {
        tspdf_stream_curve_to(s, x + bl - bl * KAPPA, y,
                               x, y + bl - bl * KAPPA,
                               x, y + bl);
    }

    tspdf_stream_close_path(s);
}

void tspdf_stream_begin_text(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "BT\n");
}

void tspdf_stream_end_text(TspdfStream *s) {
    tspdf_buffer_append_str(&s->buf, "ET\n");
}

void tspdf_stream_set_font(TspdfStream *s, const char *font_name, double size) {
    tspdf_buffer_append_byte(&s->buf, '/');
    tspdf_buffer_append_str(&s->buf, font_name);
    tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, size, 4);
    tspdf_buffer_append_str(&s->buf, " Tf\n");
}

void tspdf_stream_text_position(TspdfStream *s, double x, double y) {
    tspdf_buffer_append_double(&s->buf, x, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y, 4);
    tspdf_buffer_append_str(&s->buf, " Td\n");
}

void tspdf_stream_show_text(TspdfStream *s, const char *text) {
    tspdf_buffer_append_str(&s->buf, "(");
    for (const char *p = text; *p; p++) {
        switch (*p) {
            case '(':  tspdf_buffer_append_str(&s->buf, "\\("); break;
            case ')':  tspdf_buffer_append_str(&s->buf, "\\)"); break;
            case '\\': tspdf_buffer_append_str(&s->buf, "\\\\"); break;
            default:   tspdf_buffer_append_byte(&s->buf, (uint8_t)*p); break;
        }
    }
    tspdf_buffer_append_str(&s->buf, ") Tj\n");
}

void tspdf_stream_show_text_utf8(TspdfStream *s, const char *text,
                                TTF_Font *ttf, struct TspdfFont *pdf_font) {
    tspdf_buffer_append_str(&s->buf, "<");

    const uint8_t *p = (const uint8_t *)text;
    while (*p) {
        uint32_t codepoint;
        int bytes;

        if (*p < 0x80) {
            codepoint = *p;
            bytes = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            if (!p[1] || (p[1] & 0xC0) != 0x80) { p++; continue; }
            codepoint = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            bytes = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if (!p[1] || !p[2] || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { p++; continue; }
            codepoint = ((uint32_t)(p[0] & 0x0F) << 12) |
                        ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            bytes = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if (!p[1] || !p[2] || !p[3] ||
                (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                (p[3] & 0xC0) != 0x80) { p++; continue; }
            codepoint = ((uint32_t)(p[0] & 0x07) << 18) |
                        ((uint32_t)(p[1] & 0x3F) << 12) |
                        ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            bytes = 4;
        } else {
            p++;
            continue;
        }

        uint16_t glyph_id = ttf_get_glyph_index(ttf, codepoint);
        tspdf_buffer_printf(&s->buf, "%04X", glyph_id);

        if (pdf_font && pdf_font->glyph_to_unicode &&
            glyph_id < pdf_font->used_glyphs_count) {
            pdf_font->glyph_to_unicode[glyph_id] = codepoint;
        }

        p += bytes;
    }

    tspdf_buffer_append_str(&s->buf, "> Tj\n");
}

void tspdf_stream_set_opacity(TspdfStream *s, const char *gs_name) {
    tspdf_buffer_printf(&s->buf, "/%s gs\n", gs_name);
}

void tspdf_stream_fill_gradient(TspdfStream *s, const char *shading_name) {
    tspdf_buffer_printf(&s->buf, "/%s sh\n", shading_name);
}

void tspdf_stream_draw_image(TspdfStream *s, const char *image_name, double x, double y, double w, double h) {
    tspdf_buffer_append_str(&s->buf, "q\n");
    tspdf_buffer_append_double(&s->buf, w, 4); tspdf_buffer_append_str(&s->buf, " 0 0 ");
    tspdf_buffer_append_double(&s->buf, h, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, x, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, y, 4); tspdf_buffer_append_str(&s->buf, " cm\n");
    tspdf_buffer_append_byte(&s->buf, '/');
    tspdf_buffer_append_str(&s->buf, image_name);
    tspdf_buffer_append_str(&s->buf, " Do\nQ\n");
}

void tspdf_stream_concat_matrix(TspdfStream *s, double a, double b, double c, double d, double e, double f) {
    tspdf_buffer_append_double(&s->buf, a, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, b, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, c, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, d, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, e, 4); tspdf_buffer_append_byte(&s->buf, ' ');
    tspdf_buffer_append_double(&s->buf, f, 4);
    tspdf_buffer_append_str(&s->buf, " cm\n");
}

TspdfColor tspdf_color_rgb(double r, double g, double b) {
    return (TspdfColor){r, g, b, 1.0};
}

TspdfColor tspdf_color_rgba(double r, double g, double b, double a) {
    return (TspdfColor){r, g, b, a};
}

TspdfColor tspdf_color_from_u8(uint8_t r, uint8_t g, uint8_t b) {
    return (TspdfColor){r / 255.0, g / 255.0, b / 255.0, 1.0};
}
