#ifndef TSPDF_READER_ATTACH_H
#define TSPDF_READER_ATTACH_H

#include "tspr_internal.h"

// Embedded-file attachment preservation across extract and merge.
// Implemented in tspr_attach.c, called from tspr_document.c (same shape as
// the tspdf_doctree_* hooks).

// After set_extract_catalog: re-attach ALL source attachments onto dst
// (attachments are document-level, not page-level, so every one survives a
// page extract).
TspdfError tspdf_attach_extract_attach(TspdfReader *src, TspdfReader *dst);

// After the merge copy passes: union the sources' attachments onto the
// merged document. Name collisions resolve first-source-wins.
TspdfError tspdf_attach_merge_attach(TspdfReader *merged, TspdfReader **docs,
                                     size_t count);

#endif
