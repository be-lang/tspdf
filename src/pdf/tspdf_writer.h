#ifndef TSPDF_WRITER_H
#define TSPDF_WRITER_H

#include "pdf_writer.h"
#include "pdf_stream.h"
#include "pdf_base14.h"
#include "../font/ttf_parser.h"
#include "../font/font_subset.h"
#include "../image/jpeg_embed.h"
#include "../tspdf_error.h"
#include <stdbool.h>

#define TSPDF_MAX_PAGES_INITIAL 32
#define TSPDF_MAX_FONTS 32
#define TSPDF_MAX_IMAGES 64
#define TSPDF_MAX_OPACITY_STATES 64
#define TSPDF_MAX_ANNOTATIONS_PER_PAGE 64
#define TSPDF_MAX_BOOKMARKS 256
#define TSPDF_MAX_GRADIENTS 32
#define TSPDF_MAX_FORM_FIELDS 128

// Standard page sizes in points (1 point = 1/72 inch)
#define TSPDF_PAGE_A4_WIDTH    595.276
#define TSPDF_PAGE_A4_HEIGHT   841.890
#define TSPDF_PAGE_LETTER_WIDTH  612.0
#define TSPDF_PAGE_LETTER_HEIGHT 792.0

typedef struct {
    double width;
    double height;
} TspdfPageSize;

typedef struct {
    double x, y, w, h;    // PDF coordinates (bottom-left origin)
    char url[512];
    TspdfRef ref;            // allocated during save
} TspdfAnnotation;

typedef struct {
    TspdfRef ref;
    TspdfRef contents_ref;
    TspdfPageSize size;
    TspdfStream stream;
    TspdfAnnotation *annotations;   // dynamically allocated
    int annotation_count;
    int annotation_capacity;
} TspdfPage;

typedef struct TspdfFont {
    char name[64];            // internal name, e.g. "F1"
    char base_font_name[64];  // PDF base font name, e.g. "Helvetica"
    TspdfRef ref;
    TTF_Font *ttf;            // NULL for built-in fonts, points to loaded TTF
    const TspdfBase14Metrics *base14;  // non-NULL for built-in fonts with metrics
    bool is_builtin;
    bool *used_glyphs;        // dynamically allocated, indexed by glyph ID (size = ttf->num_glyphs)
    int used_glyphs_count;    // size of the array
    uint32_t *glyph_to_unicode;  // array indexed by glyph ID, maps to Unicode codepoint
} TspdfFont;

typedef struct {
    char title[256];
    char author[256];
    char subject[256];
    char creator[256];
    char creation_date[64];  // PDF date format: D:YYYYMMDDHHmmSS
} TspdfMetadata;

typedef struct {
    char name[16];         // e.g. "GS1"
    double fill_opacity;
    double stroke_opacity;
    TspdfRef ref;
} TspdfOpacityState;

typedef struct {
    char name[16];       // e.g. "Im1"
    TspdfRef ref;
    int width;
    int height;
    // PNG-specific: decoded pixel data (NULL for JPEG pass-through)
    uint8_t *rgb_data;     // RGB pixel data (width*height*3)
    size_t rgb_len;
    uint8_t *alpha_data;   // alpha channel (width*height), NULL if opaque
    size_t alpha_len;
    TspdfRef smask_ref;      // soft mask object ref (for alpha)
    bool is_png;
} TspdfImage;

#define TSPDF_GRADIENT_MAX_STOPS 16

typedef enum {
    TSPDF_GRADIENT_LINEAR,   // Shading Type 2 (axial)
    TSPDF_GRADIENT_RADIAL,   // Shading Type 3
} TspdfGradientType;

typedef struct {
    double position;   // 0.0 to 1.0
    TspdfColor color;
} TspdfGradientStop;

typedef struct {
    char name[16];        // e.g. "Sh1"
    TspdfRef ref;
    TspdfGradientType type;
    double x0, y0, x1, y1;  // start/end points (linear) or center/edge (radial)
    double r0, r1;           // radii (radial only)
    TspdfColor color0, color1; // legacy 2-stop colors
    TspdfGradientStop stops[TSPDF_GRADIENT_MAX_STOPS];
    int stop_count;          // 0 = use legacy color0/color1
} TspdfGradient;

// Form field types
typedef enum {
    TSPDF_FORM_TEXT,        // text input field
    TSPDF_FORM_CHECKBOX,    // checkbox (on/off)
    TSPDF_FORM_BUTTON,      // push button
} TspdfFormFieldType;

typedef struct {
    char name[64];          // field name (e.g. "first_name")
    TspdfFormFieldType type;
    int page_index;         // which page this field is on (0-based)
    double x, y, w, h;     // position in PDF coordinates (bottom-left origin)
    char default_value[256]; // default text or "Yes"/"Off" for checkbox
    char font_name[64];    // font for text fields
    double font_size;
    TspdfRef ref;
} TspdfFormField;

typedef struct {
    char title[256];
    int page_index;
    int parent;            // -1 if root-level
    int first_child;       // -1 if none
    int last_child;        // -1 if none
    int next;              // -1 if none
    int prev;              // -1 if none
    TspdfRef ref;            // allocated during save
} TspdfBookmark;

typedef struct TspdfWriter {
    TspdfRawWriter writer;

    // Pre-allocated refs
    TspdfRef catalog_ref;
    TspdfRef pages_ref;

    // Pages (dynamically allocated)
    TspdfPage *pages;
    int page_count;
    int page_capacity;

    // Fonts
    TspdfFont fonts[TSPDF_MAX_FONTS];
    int font_count;

    // Loaded TTF fonts (owned)
    TTF_Font ttf_fonts[TSPDF_MAX_FONTS];
    int ttf_font_count;

    // Images
    TspdfImage images[TSPDF_MAX_IMAGES];
    int image_count;

    // Default page size
    TspdfPageSize default_page_size;

    // Metadata
    TspdfMetadata metadata;

    // Opacity / ExtGState
    TspdfOpacityState opacity_states[TSPDF_MAX_OPACITY_STATES];
    int opacity_count;

    // Bookmarks (outline tree)
    TspdfBookmark bookmarks[TSPDF_MAX_BOOKMARKS];
    int bookmark_count;

    // Gradients (shading patterns)
    TspdfGradient gradients[TSPDF_MAX_GRADIENTS];
    int gradient_count;

    // Form fields (AcroForm)
    TspdfFormField form_fields[TSPDF_MAX_FORM_FIELDS];
    int form_field_count;

    TspdfError last_error;
    bool saved;  // guard against double-save producing corrupt PDF
} TspdfWriter;

// Create a new document (heap-allocated due to large struct size)
TspdfWriter *tspdf_writer_create(void);
void tspdf_writer_destroy(TspdfWriter *doc);

// Set default page size
void tspdf_writer_set_page_size(TspdfWriter *doc, double width, double height);

// Metadata
void tspdf_writer_set_title(TspdfWriter *doc, const char *title);
void tspdf_writer_set_author(TspdfWriter *doc, const char *author);
void tspdf_writer_set_subject(TspdfWriter *doc, const char *subject);
void tspdf_writer_set_creator(TspdfWriter *doc, const char *creator);
void tspdf_writer_set_creation_date(TspdfWriter *doc, const char *date);

// Opacity / transparency (returns the ExtGState name, e.g. "GS1")
const char *tspdf_writer_add_opacity(TspdfWriter *doc, double fill_opacity, double stroke_opacity);

// Linear gradient (returns the shading name, e.g. "Sh1")
// Coordinates are in PDF space (bottom-left origin)
const char *tspdf_writer_add_gradient(TspdfWriter *doc,
    double x0, double y0, double x1, double y1,
    TspdfColor color0, TspdfColor color1);

// Multi-stop linear gradient
const char *tspdf_writer_add_gradient_stops(TspdfWriter *doc,
    double x0, double y0, double x1, double y1,
    const TspdfGradientStop *stops, int stop_count);

// Radial gradient (2-stop)
const char *tspdf_writer_add_radial_gradient(TspdfWriter *doc,
    double cx, double cy, double r0, double r1,
    TspdfColor color0, TspdfColor color1);

// Multi-stop radial gradient
const char *tspdf_writer_add_radial_gradient_stops(TspdfWriter *doc,
    double cx, double cy, double r0, double r1,
    const TspdfGradientStop *stops, int stop_count);

// Add a new page (returns pointer to the page's content stream)
TspdfStream *tspdf_writer_add_page(TspdfWriter *doc);
TspdfStream *tspdf_writer_add_page_sized(TspdfWriter *doc, double width, double height);

// Register a built-in PDF font (Helvetica, Times-Roman, Courier, etc.)
// Returns the font name to use in content streams (e.g. "F1")
const char *tspdf_writer_add_builtin_font(TspdfWriter *doc, const char *base_font_name);

// Load and register a TrueType font from a .ttf file
// Returns the font name to use in content streams, or NULL on failure
const char *tspdf_writer_add_ttf_font(TspdfWriter *doc, const char *ttf_path);

// Load and register a TrueType font from memory (library copies the data)
const char *tspdf_writer_add_ttf_font_from_memory(TspdfWriter *doc, const uint8_t *data, size_t len);

// Get the TTF_Font* for a registered font (for text measurement)
// Returns NULL for built-in fonts
TTF_Font *tspdf_writer_get_ttf(TspdfWriter *doc, const char *font_name);

// Get the TspdfFont* for a registered font by name
TspdfFont *tspdf_writer_get_font(TspdfWriter *doc, const char *font_name);

// Get base14 metrics for a registered built-in font
// Returns NULL for TTF fonts
const TspdfBase14Metrics *tspdf_writer_get_base14(TspdfWriter *doc, const char *font_name);

// Measure text width in points using a registered font (works for both TTF and built-in)
double tspdf_writer_measure_text(TspdfWriter *doc, const char *font_name, double font_size, const char *text);

// Load a JPEG image and create a PDF XObject Image with /Filter /DCTDecode
// Returns the image name (e.g. "Im1") to use in content streams, or NULL on failure
const char *tspdf_writer_add_jpeg_image(TspdfWriter *doc, const char *jpeg_path);

// Load and embed a PNG image (decoded from scratch, no libpng)
// Returns the image name for use in content streams, or NULL on failure
const char *tspdf_writer_add_png_image(TspdfWriter *doc, const char *png_path);

// Hyperlinks / Annotations
// Add a URI link annotation to a page. Coordinates are in PDF space (bottom-left origin).
TspdfError tspdf_writer_add_link(TspdfWriter *doc, int page_index, double x, double y, double w, double h, const char *url);

// Bookmarks / Outline tree
// Add a top-level bookmark. Returns bookmark index.
int tspdf_writer_add_bookmark(TspdfWriter *doc, const char *title, int page_index);
// Add a child bookmark under parent_index. Returns bookmark index.
int tspdf_writer_add_child_bookmark(TspdfWriter *doc, int parent_index, const char *title, int page_index);

// Form fields (AcroForm)
// Add a text input field. Coordinates are PDF space (bottom-left origin).
TspdfError tspdf_writer_add_text_field(TspdfWriter *doc, int page_index,
                                  const char *name, double x, double y, double w, double h,
                                  const char *default_value, const char *font_name, double font_size);

// Add a checkbox field. checked = initial state.
TspdfError tspdf_writer_add_checkbox(TspdfWriter *doc, int page_index,
                                const char *name, double x, double y, double size, bool checked);

// Finalize and write to file
TspdfError tspdf_writer_save(TspdfWriter *doc, const char *path);

// Finalize and return PDF bytes in memory (caller must free *out_data)
TspdfError tspdf_writer_save_to_memory(TspdfWriter *doc, uint8_t **out_data, size_t *out_len);

// Get the last error that occurred on this document
TspdfError tspdf_writer_last_error(TspdfWriter *doc);

#endif
