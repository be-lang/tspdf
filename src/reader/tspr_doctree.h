#ifndef TSPDF_READER_DOCTREE_H
#define TSPDF_READER_DOCTREE_H

#include "tspr_internal.h"

// Document-level tree preservation (outlines, AcroForm) across merge and
// extract. Implemented in tspr_doctree.c, called from tspr_document.c.

// Shared object plumbing (implemented in tspr_doctree.c, also used by
// tspr_attach.c):
//
// Resolve a ref (or object number) through `doc`, covering cached objects,
// buffer-backed objects and registered new objects. Merged documents have no
// backing buffer; for them only cached/new objects resolve.
TspdfObj *tspdf_doctree_resolve_num(TspdfReader *doc, TspdfParser *parser, uint32_t num);
TspdfObj *tspdf_doctree_resolve(TspdfReader *doc, TspdfParser *parser, TspdfObj *obj);

// Append or replace `key` in an arena-backed dict.
TspdfError tspdf_obj_dict_put(TspdfObj *dict, const char *key, TspdfObj *value,
                              TspdfArena *a);

// Budget/depth-guarded /Names name-tree walk. Calls `visit` for every
// key/value pair in tree order; a visit returning false stops the walk (the
// function then also returns false, as it does on budget exhaustion).
typedef bool (*TspdfNametreeVisitFn)(void *ctx, TspdfObj *key, TspdfObj *value);
bool tspdf_nametree_walk(TspdfReader *doc, TspdfParser *parser, TspdfObj *node,
                         int depth, size_t *budget,
                         TspdfNametreeVisitFn visit, void *ctx);

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

// After the merge copy+remap passes: build the merged catalog's /Outlines
// and /AcroForm from the per-source copies. Leaves merged->catalog NULL when
// no source contributes either tree.
TspdfError tspdf_doctree_merge_attach(TspdfReader *merged, TspdfReader **docs,
                                      size_t count, const size_t *xref_offsets);

// After set_extract_catalog: attach /Outlines and /AcroForm pruned to the
// kept pages onto dst->catalog. Omits either tree entirely when nothing in
// it survives (round-1 behavior).
TspdfError tspdf_doctree_extract_attach(TspdfReader *src, TspdfReader *dst);

#endif
