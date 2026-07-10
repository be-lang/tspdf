#ifndef TSPDF_READER_TEXT_H
#define TSPDF_READER_TEXT_H

// Text extraction internals (tspr_text.c). The plain entry point is
// tspdf_reader_page_text() in tspr.h; this header adds the stats-reporting
// variant so the CLI can warn when a page's glyphs have no Unicode mapping
// (CID fonts without /ToUnicode decode to U+FFFD).

#include "tspr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t glyphs;        // glyph codes decoded on the page
    size_t replacements;  // glyphs emitted as U+FFFD (no Unicode mapping)
} TspdfTextStats;

// Same contract as tspdf_reader_page_text; additionally fills *stats when
// stats is non-NULL.
const char *tspdf_reader_page_text_stats(TspdfReader *doc, size_t page_index,
                                         TspdfTextStats *stats, TspdfError *err);

// Same contract as tspdf_reader_page_text_layout (tspr.h) plus stats.
const char *tspdf_reader_page_text_layout_stats(TspdfReader *doc, size_t page_index,
                                                TspdfTextStats *stats, TspdfError *err);

#ifdef __cplusplus
}
#endif

#endif
