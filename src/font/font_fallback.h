#ifndef TSPDF_FONT_FALLBACK_H
#define TSPDF_FONT_FALLBACK_H

// System-font discovery for the form-fill fallback font (no bundled font:
// zero dependencies and no font licensing). Given the set of Unicode code
// points a field value needs, find a TrueType font on this machine whose
// cmap actually covers them and whose outlines are glyf-based (the subsetter
// cannot process CFF 'OTTO' fonts, so those are skipped).
//
// Lookup order:
//   1. TSPDF_FALLBACK_FONT=<path>  explicit override; used only when it
//      parses and covers the code points (no silent scan behind it).
//   2. TSPDF_FONT_DIRS=<dir:dir>   replaces the built-in scan roots.
//   3. Built-in roots: /usr/share/fonts, /usr/local/share/fonts, ~/.fonts,
//      ~/.local/share/fonts, /System/Library/Fonts, /Library/Fonts —
//      scanned recursively (bounded depth and file count). Candidates are
//      ordered by cheap filename hints (CJK/Noto/DejaVu/... before others)
//      and then parsed one by one until one covers the code points.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Find a usable fallback font for `cps`. Returns a malloc'd path (caller
// frees) or NULL when nothing on this machine covers the code points.
char *tspdf_fallback_font_find(const uint32_t *cps, size_t count);

// True when the file at `path` parses as a TrueType font with glyf outlines
// and maps every code point in `cps` to a real glyph.
bool tspdf_fallback_font_covers(const char *path, const uint32_t *cps,
                                size_t count);

// Filename ranking hint for candidate ordering (higher = parsed earlier).
// `want_cjk` biases CJK-named files to the front. Exposed for tests.
int tspdf_fallback_font_score(const char *filename, bool want_cjk);

#ifdef __cplusplus
}
#endif

#endif
