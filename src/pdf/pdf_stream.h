#ifndef TSPDF_STREAM_H
#define TSPDF_STREAM_H

#include "../util/buffer.h"
#include "../font/ttf_parser.h"

// Forward declare TspdfFont (full definition in tspdf_writer.h)
struct TspdfFont;

// Builds a PDF content stream (sequence of drawing operators)
typedef struct TspdfStream {
    TspdfBuffer buf;
} TspdfStream;

typedef struct {
    double r, g, b, a;  // 0.0 - 1.0
} TspdfColor;

TspdfStream tspdf_stream_create(void);
void tspdf_stream_destroy(TspdfStream *s);

// Graphics state
void tspdf_stream_save(TspdfStream *s);     // q
void tspdf_stream_restore(TspdfStream *s);  // Q

// Color (non-stroking = fill, stroking = stroke)
void tspdf_stream_set_fill_color(TspdfStream *s, TspdfColor c);
void tspdf_stream_set_stroke_color(TspdfStream *s, TspdfColor c);
void tspdf_stream_set_line_width(TspdfStream *s, double w);
void tspdf_stream_set_dash(TspdfStream *s, const double *pattern, int count, double phase);

// Path construction
void tspdf_stream_move_to(TspdfStream *s, double x, double y);
void tspdf_stream_line_to(TspdfStream *s, double x, double y);
void tspdf_stream_curve_to(TspdfStream *s, double x1, double y1, double x2, double y2, double x3, double y3);
void tspdf_stream_rect(TspdfStream *s, double x, double y, double w, double h);
void tspdf_stream_close_path(TspdfStream *s);  // h

// Path painting
void tspdf_stream_fill(TspdfStream *s);         // f
void tspdf_stream_stroke(TspdfStream *s);       // S
void tspdf_stream_fill_stroke(TspdfStream *s);  // B
void tspdf_stream_clip(TspdfStream *s);         // W n

// Rounded rectangle (y-down coordinate system, for direct use)
void tspdf_stream_rounded_rect(TspdfStream *s, double x, double y, double w, double h,
                             double tl, double tr, double br, double bl);

// Rounded rectangle in PDF coordinates (y-up, (x,y) is bottom-left)
// tl/tr/br/bl are visual corners (top-left, top-right, bottom-right, bottom-left)
void tspdf_stream_rounded_rect_pdf(TspdfStream *s, double x, double y, double w, double h,
                                 double tl, double tr, double br, double bl);

// Text
void tspdf_stream_begin_text(TspdfStream *s);   // BT
void tspdf_stream_end_text(TspdfStream *s);     // ET
void tspdf_stream_set_font(TspdfStream *s, const char *font_name, double size);  // Tf
void tspdf_stream_text_position(TspdfStream *s, double x, double y);  // Td
void tspdf_stream_show_text(TspdfStream *s, const char *text);  // Tj
void tspdf_stream_show_text_utf8(TspdfStream *s, const char *text,
                                TTF_Font *ttf, struct TspdfFont *pdf_font);

// Opacity (writes /GS_name gs operator; name must be registered with tspdf_writer_add_opacity)
void tspdf_stream_set_opacity(TspdfStream *s, const char *gs_name);

// Gradients
// Fills the current clipping region with a shading pattern (sh operator)
void tspdf_stream_fill_gradient(TspdfStream *s, const char *shading_name);

// Images
// Draws an image at (x, y) with dimensions (w, h) using cm + Do operators
// image_name is the name returned by tspdf_writer_add_jpeg_image (e.g. "Im1")
void tspdf_stream_draw_image(TspdfStream *s, const char *image_name, double x, double y, double w, double h);

// Transform
void tspdf_stream_concat_matrix(TspdfStream *s, double a, double b, double c, double d, double e, double f);  // cm

// Helpers
TspdfColor tspdf_color_rgb(double r, double g, double b);
TspdfColor tspdf_color_rgba(double r, double g, double b, double a);
TspdfColor tspdf_color_from_u8(uint8_t r, uint8_t g, uint8_t b);

#endif
