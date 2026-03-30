#ifndef TSPDF_LAYOUT_H
#define TSPDF_LAYOUT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../tspdf_error.h"
#include "../pdf/pdf_stream.h"

#define TSPDF_LAYOUT_MAX_CHILDREN 32
#define TSPDF_LAYOUT_MAX_TEXT     1024
#define TSPDF_TSPDF_LAYOUT_MAX_TEXT_LINES 64

// Typographic defaults (used when no font metrics callback is set)
#define TSPDF_LAYOUT_DEFAULT_LINE_HEIGHT_FACTOR 1.2
#define TSPDF_LAYOUT_DEFAULT_ASCENT_FACTOR      0.75
#define TSPDF_LAYOUT_FALLBACK_CHAR_WIDTH_FACTOR 0.6

// --- Sizing ---

typedef enum {
    TSPDF_SIZE_FIXED,    // exact pixel/point value
    TSPDF_SIZE_FIT,      // shrink to fit content
    TSPDF_SIZE_GROW,     // expand to fill parent
    TSPDF_SIZE_PERCENT,  // percentage of parent (0.0 - 1.0)
} TspdfSizeMode;

typedef struct {
    TspdfSizeMode mode;
    double value;
} TspdfSize;

// --- TspdfDirection and alignment ---

typedef enum {
    TSPDF_DIR_ROW,       // left to right
    TSPDF_DIR_COLUMN,    // top to bottom
} TspdfDirection;

typedef enum {
    TSPDF_ALIGN_START,
    TSPDF_ALIGN_CENTER,
    TSPDF_ALIGN_END,
} TspdfAlignment;

typedef enum {
    TSPDF_TEXT_ALIGN_LEFT,
    TSPDF_TEXT_ALIGN_CENTER,
    TSPDF_TEXT_ALIGN_RIGHT,
} TspdfTextAlignment;

typedef enum {
    TSPDF_WRAP_NONE,     // no wrapping, single line
    TSPDF_WRAP_WORD,     // wrap at word boundaries
    TSPDF_WRAP_CHAR,     // wrap at character boundaries
} TspdfWrapMode;

// --- TspdfPadding ---

typedef struct {
    double top, right, bottom, left;
} TspdfPadding;

// --- Repeat mode (page filters for paginated layout) ---

typedef enum {
    TSPDF_REPEAT_NONE = 0,
    TSPDF_REPEAT_ALL = 1,        // every page
    TSPDF_REPEAT_FIRST = 2,      // first page only (non-repeating, just marks it)
    TSPDF_REPEAT_NOT_FIRST = 4,  // every page except first
    TSPDF_REPEAT_EVEN = 8,       // even pages (0-indexed: pages 0, 2, 4...)
    TSPDF_REPEAT_ODD = 16,       // odd pages (0-indexed: pages 1, 3, 5...)
} TspdfRepeatMode;

// --- Node types ---

typedef enum {
    TSPDF_NODE_BOX,
    TSPDF_NODE_TEXT,
} TspdfNodeType;

// Forward declare
typedef struct TspdfNode TspdfNode;

// Text decoration
typedef enum {
    TSPDF_TEXT_DECOR_NONE        = 0,
    TSPDF_TEXT_DECOR_UNDERLINE   = 1,
    TSPDF_TEXT_DECOR_STRIKETHROUGH = 2,
    TSPDF_TEXT_DECOR_OVERLINE    = 4,
} TspdfTextDecoration;

// Rich text span (for inline formatting within a text node)
#define TSPDF_LAYOUT_MAX_SPANS 4

typedef struct {
    const char *text;       // arena-allocated
    const char *font_name;  // arena-allocated
    double font_size;
    TspdfColor color;
    int decoration;   // bitmask of TspdfTextDecoration
} TspdfTextSpan;

// Text config
typedef struct {
    const char *text;       // arena-allocated
    const char *font_name;  // arena-allocated
    double font_size;
    TspdfColor color;
    TspdfTextAlignment alignment;
    TspdfWrapMode wrap;
    double line_height_factor;  // multiplier on font line height (default 1.2)
    int decoration;             // bitmask of TspdfTextDecoration
    // Rich text spans (if span_count > 0, these override the single text/font fields)
    TspdfTextSpan *spans;    // realloc'd array, NULL until first span added
    int span_count;
    int span_capacity;
} TspdfTextConfig;

// Box config
typedef struct {
    TspdfColor background;
    bool has_background;
    double corner_radius[4];  // tl, tr, br, bl
    TspdfColor border_color;
    double border_width;
    bool has_border;
    // Per-side borders (override border_width/border_color when set)
    double border_top, border_right, border_bottom, border_left;
    TspdfColor border_color_top, border_color_right, border_color_bottom, border_color_left;
    bool has_border_top, has_border_right, has_border_bottom, has_border_left;
    // Dash pattern: 0=solid (default), dash_on/dash_off in points
    double dash_on, dash_off;
    // Drop shadow
    bool has_shadow;
    TspdfColor shadow_color;
    double shadow_offset_x, shadow_offset_y;  // offset in points (positive = right/down)
    double shadow_blur;                        // simulated blur via multiple offset rects
    // Background image (image_name from tspdf_writer_add_jpeg_image)
    const char *bg_image;   // NULL = no background image
} TspdfBoxStyle;

// --- Vector path commands ---

#define TSPDF_PATH_MAX_COMMANDS 64

typedef enum {
    TSPDF_PATH_MOVE_TO,
    TSPDF_PATH_LINE_TO,
    TSPDF_PATH_CURVE_TO,    // cubic Bezier
    TSPDF_PATH_CLOSE,
    TSPDF_PATH_ARC,         // circular arc (converted to Bezier curves)
} TspdfPathCmdType;

typedef struct {
    TspdfPathCmdType type;
    double x, y;             // endpoint (or center for arc)
    double cx1, cy1;         // control point 1 (curve_to)
    double cx2, cy2;         // control point 2 (curve_to)
    double radius;           // for arc
    double start_angle;      // for arc (degrees)
    double sweep_angle;      // for arc (degrees)
} TspdfPathCmd;

typedef struct {
    TspdfPathCmd commands[TSPDF_PATH_MAX_COMMANDS];
    int count;
    TspdfColor fill_color;
    TspdfColor stroke_color;
    double stroke_width;
    bool has_fill;
    bool has_stroke;
} TspdfPathConfig;

// A layout node
struct TspdfNode {
    TspdfNodeType type;

    // Sizing
    TspdfSize width;
    TspdfSize height;

    // Layout behavior (for boxes with children)
    TspdfDirection direction;
    TspdfAlignment align_x;
    TspdfAlignment align_y;
    double gap;
    int list_type;  // TspdfListType: 0 = not a list, TSPDF_LIST_BULLET, TSPDF_LIST_NUMBERED
    TspdfPadding padding;

    // Style (NULL = no styling; arena-allocated when set)
    TspdfBoxStyle *style;

    // Text (only for TSPDF_NODE_TEXT)
    TspdfTextConfig text;

    // Vector path (optional custom drawing within this node's bounds)
    TspdfPathConfig *path;  // NULL = no custom path

    // Tree structure
    TspdfNode **children;  // arena-allocated array, grown on demand
    int child_count;
    int child_capacity;
    TspdfNode *parent;

    // Transforms
    double rotation;      // degrees, rotation around element center
    double scale_x;       // horizontal scale (0 = use default 1.0)
    double scale_y;       // vertical scale (0 = use default 1.0)

    // Clipping
    bool clip_children;   // if true, children are clipped to this node's bounds

    // Pagination
    TspdfRepeatMode repeat_mode;  // controls on which pages this element appears when paginated

    // Computed layout (filled by tspdf_layout_compute)
    double computed_x;
    double computed_y;
    double computed_width;
    double computed_height;

    // For text nodes: computed wrapped lines (arena-allocated arrays)
    struct {
        char **lines;
        double *line_widths;
        int line_count;
        int line_capacity;
    } computed_text;
};

// --- Construction helpers ---

static inline TspdfSize tspdf_size_fixed(double v) { return (TspdfSize){TSPDF_SIZE_FIXED, v}; }
static inline TspdfSize tspdf_size_fit(void) { return (TspdfSize){TSPDF_SIZE_FIT, 0}; }
static inline TspdfSize tspdf_size_grow(void) { return (TspdfSize){TSPDF_SIZE_GROW, 1}; }
static inline TspdfSize tspdf_size_grow_weight(double w) { return (TspdfSize){TSPDF_SIZE_GROW, w}; }
static inline TspdfSize tspdf_size_percent(double p) { return (TspdfSize){TSPDF_SIZE_PERCENT, p}; }

static inline TspdfPadding tspdf_padding_all(double v) { return (TspdfPadding){v, v, v, v}; }
static inline TspdfPadding tspdf_padding_xy(double x, double y) { return (TspdfPadding){y, x, y, x}; }
static inline TspdfPadding tspdf_padding_each(double t, double r, double b, double l) {
    return (TspdfPadding){t, r, b, l};
}

// --- Layout context (replaces global state) ---

#include "../util/arena.h"

// Text measurement callback types
typedef double (*TspdfMeasureTextFn)(const char *font_name, double font_size, const char *text, void *userdata);
typedef double (*TspdfFontLineHeightFn)(const char *font_name, double font_size, void *userdata);

typedef struct {
    TspdfArena *arena;
    TspdfMeasureTextFn measure_text;
    void *measure_userdata;
    TspdfFontLineHeightFn font_line_height;
    void *line_height_userdata;
    void *doc;  // TspdfWriter pointer for font lookup during rendering (optional)
} TspdfLayout;

// Create a layout context bound to an arena
TspdfLayout tspdf_layout_create(TspdfArena *arena);

// --- TspdfArena-based node allocation ---

// Create nodes
TspdfNode *tspdf_layout_box(TspdfLayout *ctx);
TspdfNode *tspdf_layout_text(TspdfLayout *ctx, const char *text, const char *font_name, double font_size);

// Add a child to a parent
TspdfError tspdf_layout_add_child(TspdfNode *parent, TspdfNode *child);

// Add a rich text span to a text node
bool tspdf_layout_text_add_span(TspdfNode *node, const char *text,
                           const char *font_name, double font_size,
                           TspdfColor color, int decoration);

// Get or allocate the style for a node (arena-allocated on first call)
TspdfBoxStyle *tspdf_layout_node_style(TspdfLayout *ctx, TspdfNode *node);

// Free realloc'd arrays (children, spans, computed_text) in a layout tree.
// Must be called before tspdf_arena_destroy() to avoid memory leaks.
void tspdf_layout_tree_free(TspdfNode *root);

// --- Compute layout ---
// root_width/root_height: available space
void tspdf_layout_compute(TspdfLayout *ctx, TspdfNode *root, double root_width, double root_height);

// --- Render to PDF stream ---
void tspdf_layout_render(TspdfLayout *ctx, TspdfNode *root, TspdfStream *stream);

// --- Paginated layout ---
// For content that overflows a single page.
// Root must be TSPDF_DIR_COLUMN. Children with repeat_mode != TSPDF_REPEAT_NONE
// appear at the top of pages based on their repeat filter (e.g. table headers).

#define TSPDF_LAYOUT_MAX_PAGINATED_PAGES 256
#define TSPDF_LAYOUT_MAX_PAGINATED_ITEMS 2048  // max content/repeat items across all pages

typedef struct {
    int content_indices[TSPDF_LAYOUT_MAX_PAGINATED_ITEMS];  // indices of non-repeating children
    int content_count;
    int repeat_indices[TSPDF_LAYOUT_MAX_PAGINATED_ITEMS];   // indices of repeating children
    int repeat_count;
    double repeat_height;                       // total height of repeat section + gap
    struct {
        int start;  // index into content_indices[]
        int end;    // past-end index into content_indices[]
    } pages[TSPDF_LAYOUT_MAX_PAGINATED_PAGES];
    int page_count;
    double page_width;
    double page_height;
} TspdfPaginationResult;

// Compute paginated layout. Returns number of pages.
int tspdf_layout_compute_paginated(TspdfLayout *ctx, TspdfNode *root, double page_width, double page_height,
                              TspdfPaginationResult *result);

// Render a specific page (0-indexed) from paginated layout.
void tspdf_layout_render_page(TspdfLayout *ctx, TspdfNode *root, const TspdfPaginationResult *result,
                         int page_index, TspdfStream *stream);

// Render a page with GROW-aware recompute — re-runs sizing for each page's
// children with the actual available height, so GROW elements fill correctly.
void tspdf_layout_render_page_recompute(TspdfLayout *ctx, TspdfNode *root, const TspdfPaginationResult *result,
                                   int page_index, TspdfStream *stream);


// --- Page number rendering ---
// Draws "Page X of Y" text on a page. align controls horizontal position.
// margin_bottom is distance from bottom of page in points.
void tspdf_layout_render_page_number(TspdfLayout *ctx, TspdfStream *stream, int page_index, int total_pages,
                                const char *font_name, double font_size,
                                TspdfColor color, double page_width, double page_height,
                                TspdfTextAlignment align, double margin_bottom);

// --- Table helper ---

typedef struct {
    const char *font_name;
    double font_size;
    TspdfColor text_color;
    TspdfColor header_bg;
    TspdfColor header_text_color;
    TspdfColor row_bg_even;
    TspdfColor row_bg_odd;
    TspdfColor border_color;
    double border_width;
    double row_height;
    double header_height;
    double padding;
} TspdfTableStyle;

// Create a table. columns is an array of header strings, col_widths is percentage (0-1) per column.
// Returns the table root node. Add rows with tspdf_layout_table_add_row().
TspdfNode *tspdf_layout_table(TspdfLayout *ctx, const char **columns, const double *col_widths, int col_count, TspdfTableStyle style);

// Add a row of text cells to a table. cells is an array of strings (one per column).
// col_widths must match the widths used when creating the table.
void tspdf_layout_table_add_row(TspdfLayout *ctx, TspdfNode *table, const char **cells, const double *col_widths,
                           int cell_count, TspdfTableStyle style);

// Add a row with colspan support. colspans[i] = number of columns cell i spans.
// Sum of all colspans must equal col_count. cell_count is the number of cells (not columns).
void tspdf_layout_table_add_row_spans(TspdfLayout *ctx, TspdfNode *table, const char **cells, const int *colspans,
                                 int cell_count, const double *col_widths, int col_count,
                                 TspdfTableStyle style);

// Compute auto column widths by measuring all cell content.
// Writes proportional widths (summing to 1.0) into col_widths_out[].
// col_widths_out must have space for col_count doubles.
void tspdf_layout_table_compute_widths(TspdfLayout *ctx, const char **headers,
                                  const char **rows_flat, int row_count,
                                  int col_count, TspdfTableStyle style,
                                  double *col_widths_out);

// Create a complete table with auto-sized columns.
// rows_flat is a flat array of strings: rows_flat[row * col_count + col].
TspdfNode *tspdf_layout_table_auto(TspdfLayout *ctx, const char **headers,
                               const char **rows_flat, int row_count,
                               int col_count, TspdfTableStyle style);

// --- List helper ---

typedef enum {
    TSPDF_LIST_BULLET,
    TSPDF_LIST_NUMBERED,
} TspdfListType;

// Create a list. Returns the list root node. Add items with tspdf_layout_list_add_item().
TspdfNode *tspdf_layout_list(TspdfLayout *ctx, TspdfListType type, const char *font_name, double font_size, TspdfColor color);

// Add a text item to the list.
void tspdf_layout_list_add_item(TspdfLayout *ctx, TspdfNode *list, const char *text);

// --- Vector path builder ---
// Creates a path config on a box node. Coordinates are relative to the node's bounds
// (0,0 = top-left, width/height = bottom-right in layout space).

// Initialize a path on a node (allocates from arena)
TspdfPathConfig *tspdf_layout_path_begin(TspdfLayout *ctx, TspdfNode *node);

// Path commands (coordinates relative to node bounds, y-down)
bool tspdf_layout_path_move_to(TspdfPathConfig *p, double x, double y);
bool tspdf_layout_path_line_to(TspdfPathConfig *p, double x, double y);
bool tspdf_layout_path_curve_to(TspdfPathConfig *p, double x1, double y1, double x2, double y2, double x3, double y3);
bool tspdf_layout_path_close(TspdfPathConfig *p);
bool tspdf_layout_path_arc(TspdfPathConfig *p, double cx, double cy, double radius, double start_deg, double sweep_deg);

// Set fill/stroke
void tspdf_layout_path_set_fill(TspdfPathConfig *p, TspdfColor color);
void tspdf_layout_path_set_stroke(TspdfPathConfig *p, TspdfColor color, double width);

#endif
