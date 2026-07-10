#ifndef TSPDF_FONT_SUBSET_H
#define TSPDF_FONT_SUBSET_H

#include "ttf_parser.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a subset TTF that contains only the specified glyphs.
// used_glyphs is a boolean array indexed by glyph ID (size = font->num_glyphs).
// Glyph 0 (.notdef) is always included.
// Returns malloc'd buffer with the subset TTF, sets *out_len.
// Returns NULL on failure.
uint8_t *ttf_subset(const TTF_Font *font, const bool *used_glyphs, size_t *out_len);

// Build the /ToUnicode CMap stream content for an Identity-H CIDFontType2
// (CID == glyph ID): one bfchar entry per used glyph with a non-zero Unicode
// mapping in glyph_to_unicode. Returns a malloc'd buffer (caller frees),
// sets *out_len; NULL on allocation failure. Shared between the writer's
// TTF embedding and the reader's form fallback-font embedding.
uint8_t *tspdf_font_tounicode_cmap(const bool *used_glyphs,
                                   const uint32_t *glyph_to_unicode,
                                   int glyph_count, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
