#ifndef TSPDF_TTF_PARSER_H
#define TSPDF_TTF_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TSPDF_TTF_MAX_GLYPHS 65536

// Kerning pair
typedef struct {
    uint16_t left;
    uint16_t right;
    int16_t  value;
} TTF_KernPair;

// TrueType table directory entry
typedef struct {
    uint32_t tag;
    uint32_t checksum;
    uint32_t offset;
    uint32_t length;
} TTF_TableEntry;

// Horizontal metrics for a single glyph
typedef struct {
    uint16_t advance_width;
    int16_t  lsb;  // left side bearing
} TTF_HMetric;

// Parsed TrueType font
typedef struct {
    // Raw file data (owned)
    uint8_t *data;
    size_t   data_len;

    // Table directory
    TTF_TableEntry *tables;
    uint16_t num_tables;

    // head table
    uint16_t units_per_em;
    int16_t  x_min, y_min, x_max, y_max;  // font bounding box
    int16_t  index_to_loc_format;          // 0=short, 1=long

    // hhea table
    int16_t  ascent;
    int16_t  descent;
    int16_t  line_gap;
    uint16_t num_h_metrics;

    // maxp table
    uint16_t num_glyphs;

    // hmtx table (array of num_glyphs entries)
    TTF_HMetric *h_metrics;

    // cmap: we store the offset to the selected subtable for lookups
    uint32_t cmap_offset;  // offset into data for the selected subtable
    uint32_t cmap_length;  // length of the cmap subtable (for bounds checking)
    int      cmap_format;

    // name table
    char font_family[256];
    char font_subfamily[256];
    char full_name[256];
    char postscript_name[256];

    // os2 table (if present)
    bool     has_os2;
    int16_t  os2_typo_ascender;
    int16_t  os2_typo_descender;
    int16_t  os2_typo_line_gap;
    uint16_t os2_weight_class;
    int16_t  os2_cap_height;
    int16_t  os2_x_height;

    // kern table
    TTF_KernPair *kern_pairs;
    int kern_pair_count;
    bool has_kern;

    // loca table offset and glyf table offset (for future subsetting)
    uint32_t loca_offset;
    uint32_t loca_length;
    uint32_t glyf_offset;
    uint32_t glyf_length;
} TTF_Font;

// Load a TTF font from a file path
bool ttf_load(TTF_Font *font, const char *path);

// Load from memory (takes ownership of data)
bool ttf_load_from_memory(TTF_Font *font, uint8_t *data, size_t len);

// Free font resources
void ttf_free(TTF_Font *font);

// Map a Unicode codepoint to a glyph index
uint16_t ttf_get_glyph_index(const TTF_Font *font, uint32_t codepoint);

// Get kerning value between two glyphs (in font units)
int16_t ttf_get_kerning(TTF_Font *font, uint16_t left_glyph, uint16_t right_glyph);

// Get advance width for a glyph (in font units)
uint16_t ttf_get_glyph_advance(const TTF_Font *font, uint16_t glyph_index);

// Get advance width for a Unicode codepoint (in font units)
uint16_t ttf_get_char_advance(const TTF_Font *font, uint32_t codepoint);

// Measure a string width in font units
int ttf_measure_string(const TTF_Font *font, const char *text);

// Convert font units to points at a given font size
double ttf_units_to_points(const TTF_Font *font, int font_units, double font_size);

// Get scaled metrics
double ttf_get_ascent(const TTF_Font *font, double font_size);
double ttf_get_descent(const TTF_Font *font, double font_size);
double ttf_get_line_height(const TTF_Font *font, double font_size);

// Shared helper: build a 4-byte TrueType tag from a string
static inline uint32_t make_tag(const char *s) {
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8) | (uint32_t)s[3];
}

#endif
