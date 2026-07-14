#ifndef TSPDF_VERSION_H
#define TSPDF_VERSION_H

// Single source of truth for the tspdf version. The Makefile parses the three
// numeric macros below (see `make print-version`) so the build metadata and the
// compiled-in constants can never drift apart.

#define TSPDF_VERSION_MAJOR 0
#define TSPDF_VERSION_MINOR 5
#define TSPDF_VERSION_PATCH 1

#define TSPDF_STRINGIFY_(x) #x
#define TSPDF_STRINGIFY(x) TSPDF_STRINGIFY_(x)

#define TSPDF_VERSION_STRING \
    TSPDF_STRINGIFY(TSPDF_VERSION_MAJOR) "." \
    TSPDF_STRINGIFY(TSPDF_VERSION_MINOR) "." \
    TSPDF_STRINGIFY(TSPDF_VERSION_PATCH)

// Numeric form for compile-time comparisons: (MAJOR*10000 + MINOR*100 + PATCH).
#define TSPDF_VERSION_NUMBER \
    (TSPDF_VERSION_MAJOR * 10000 + TSPDF_VERSION_MINOR * 100 + TSPDF_VERSION_PATCH)

#ifdef __cplusplus
extern "C" {
#endif

// Returns the compiled-in version string, e.g. "0.1.0". Header-only so the
// zero-dependency library needs no extra translation unit.
static inline const char *tspdf_version(void) {
    return TSPDF_VERSION_STRING;
}

#ifdef __cplusplus
}
#endif

#endif
