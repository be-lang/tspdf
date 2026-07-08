#include "tspr_internal.h"
#include "tspr_text.h"

// Stub: implementation follows the failing tests (TDD).
const char *tspdf_reader_page_text_stats(TspdfReader *doc, size_t page_index,
                                         TspdfTextStats *stats, TspdfError *err) {
    (void)doc; (void)page_index;
    if (stats) { stats->glyphs = 0; stats->replacements = 0; }
    if (err) *err = TSPDF_ERR_UNSUPPORTED;
    return NULL;
}

const char *tspdf_reader_page_text(TspdfReader *doc, size_t page_index, TspdfError *err) {
    return tspdf_reader_page_text_stats(doc, page_index, NULL, err);
}
