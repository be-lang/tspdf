#ifndef TSPDF_FONT_SUBSET_H
#define TSPDF_FONT_SUBSET_H

#include "ttf_parser.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Build a subset TTF that contains only the specified glyphs.
// used_glyphs is a boolean array indexed by glyph ID (size = font->num_glyphs).
// Glyph 0 (.notdef) is always included.
// Returns malloc'd buffer with the subset TTF, sets *out_len.
// Returns NULL on failure.
uint8_t *ttf_subset(const TTF_Font *font, const bool *used_glyphs, size_t *out_len);

#endif
