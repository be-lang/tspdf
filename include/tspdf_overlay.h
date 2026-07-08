#ifndef TSPDF_OVERLAY_H_PUBLIC
#define TSPDF_OVERLAY_H_PUBLIC

// Content overlay API for drawing on existing PDF pages.
// Separate from tspdf.h because it pulls in the full writer internals.
//
// Works in the same two layouts as tspdf.h (see the comment there): in-tree
// via relative "../src/..." includes, or installed under
// $(PREFIX)/include/tspdf/ where the TSPDF_INSTALLED marker header switches
// the includes to the <tspdf/...> form.

#if defined(__has_include) && !defined(TSPDF_INSTALLED)
#  if __has_include(<tspdf/tspdf_installed.h>)
#    define TSPDF_INSTALLED 1
#  endif
#endif

#ifdef TSPDF_INSTALLED

#include <tspdf/tspdf.h>
#include <tspdf/reader/tspr_overlay.h>

#else  // in-tree build

#include "tspdf.h"
#include "../src/reader/tspr_overlay.h"

#endif  // TSPDF_INSTALLED

#endif
