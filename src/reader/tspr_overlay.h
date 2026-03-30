#ifndef TSPDF_OVERLAY_H
#define TSPDF_OVERLAY_H

// Phase 3: Content overlay API for drawing on existing PDF pages.
// Separated from tspr.h because it pulls in the full tspdf writer API
// (TspdfWriter, TspdfStream). Include this only if you need overlay support.

#include "tspr.h"
#include "../pdf/pdf_stream.h"
#include "../pdf/tspdf_writer.h"

// Begin a content overlay on an existing page. Returns a TspdfStream you can
// draw into using the standard tspdf_stream_* API. Call tspdf_page_end_content
// to finalize or tspdf_page_abort_content to discard.
TspdfStream *tspdf_page_begin_content(TspdfReader *doc, size_t page_index);
TspdfError tspdf_page_end_content(TspdfReader *doc, size_t page_index,
                                 TspdfStream *stream, TspdfWriter *resource_owner);
void tspdf_page_abort_content(TspdfStream *stream);

#endif
