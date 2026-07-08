#ifndef TSPDF_BASE14_H
#define TSPDF_BASE14_H

#include <stdint.h>

// Metrics for the 14 standard PDF fonts (from AFM files).
// Widths are in units of 1/1000 em, indexed by WinAnsiEncoding code point.
// Symbol and ZapfDingbats use their own built-in encodings.

typedef struct {
    const char *name;
    int16_t ascent;      // units of 1/1000 em
    int16_t descent;     // negative
    int16_t cap_height;
    int16_t x_height;
    uint16_t widths[256];
} TspdfBase14Metrics;

// Look up metrics by base font name (e.g. "Helvetica", "Times-Roman").
// Returns NULL if not a standard font.
const TspdfBase14Metrics *tspdf_base14_get(const char *font_name);

// Measure a string's width in points using base14 metrics.
double tspdf_base14_measure_text(const TspdfBase14Metrics *metrics, double font_size, const char *text);

// Get line height in points.
double tspdf_base14_line_height(const TspdfBase14Metrics *metrics, double font_size);

// Returns true if the font uses WinAnsiEncoding (all except Symbol, ZapfDingbats).
int tspdf_base14_is_latin(const char *font_name);

#endif
