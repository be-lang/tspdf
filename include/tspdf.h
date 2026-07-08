#ifndef TSPDF_H
#define TSPDF_H

// tspdf — PDF toolkit in pure C. Zero dependencies.
//
// This umbrella header works in two layouts:
//   * In-tree:   include/tspdf.h alongside a sibling src/ tree (relative "../src/...").
//   * Installed: $(PREFIX)/include/tspdf/tspdf.h with the internal headers installed
//                under the same tspdf/ prefix (see `make install`), so consumers use
//                `#include <tspdf/tspdf.h>` and the copy is fully relocatable.
//
// The installed copy defines TSPDF_INSTALLED (via a marker header dropped next to it
// at install time) so the includes resolve under <tspdf/...> instead of "../src/...".

#if defined(__has_include)
#  if __has_include(<tspdf/tspdf_installed.h>)
#    define TSPDF_INSTALLED 1
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TSPDF_INSTALLED

// Version / API
#include <tspdf/version.h>

// Error handling
#include <tspdf/tspdf_error.h>

// PDF generation (writer)
#include <tspdf/pdf/tspdf_writer.h>
#include <tspdf/pdf/pdf_stream.h>

// PDF reading and manipulation (reader)
#include <tspdf/reader/tspr.h>

// Layout engine
#include <tspdf/layout/layout.h>

// Memory management
#include <tspdf/util/arena.h>

#else  // in-tree build

// Version / API
#include "tspdf/version.h"

// Error handling
#include "../src/tspdf_error.h"

// PDF generation (writer)
#include "../src/pdf/tspdf_writer.h"
#include "../src/pdf/pdf_stream.h"

// PDF reading and manipulation (reader)
#include "../src/reader/tspr.h"

// Layout engine
#include "../src/layout/layout.h"

// Memory management
#include "../src/util/arena.h"

#endif  // TSPDF_INSTALLED

#ifdef __cplusplus
}
#endif

#endif
