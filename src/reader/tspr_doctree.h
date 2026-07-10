#ifndef TSPDF_READER_DOCTREE_H
#define TSPDF_READER_DOCTREE_H

#include "tspr_internal.h"

// Document-level tree preservation (outlines, AcroForm) across merge and
// extract. Implemented in tspr_doctree.c, called from tspr_document.c.

// Exported from tspr_document.c: recursive stream self-containment used by
// merge preparation (merged documents have no single backing buffer).
TspdfError tspdf_reader_make_ref_self_contained(uint32_t obj_num, const uint8_t *src_data, size_t src_len,
                                                TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                                TspdfCrypt *crypt, bool *visited, size_t xref_count);
TspdfError tspdf_reader_make_streams_self_contained(TspdfObj *obj, const uint8_t *src_data, size_t src_len,
                                                    TspdfObj **cache, TspdfReaderXref *xref, TspdfParser *parser,
                                                    TspdfCrypt *crypt, bool *visited, size_t xref_count);

// Resolve a source's outline items and AcroForm graph into src->obj_cache
// (self-containing any streams) so the merge copy loop picks them up.
// Deliberately does not walk outline /SE entries: those would drag the whole
// structure tree into the merged document.
TspdfError tspdf_doctree_merge_prepare(TspdfReader *src, TspdfParser *parser,
                                       bool *visited);

// After the merge copy+remap passes: build the merged catalog's /Outlines,
// /AcroForm, and /PageLabels from the per-source copies. Leaves
// merged->catalog NULL when no source contributes any of them.
TspdfError tspdf_doctree_merge_attach(TspdfReader *merged, TspdfReader **docs,
                                      size_t count, const size_t *xref_offsets);

// After set_extract_catalog: attach /Outlines and /AcroForm pruned to the
// kept pages onto dst->catalog, plus a /PageLabels tree rebuilt so each
// kept page keeps its effective source label. Omits any tree entirely when
// nothing in it survives (round-1 behavior).
TspdfError tspdf_doctree_extract_attach(TspdfReader *src, TspdfReader *dst);

#endif
