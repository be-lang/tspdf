#ifndef TSOPS_OPS_H
#define TSOPS_OPS_H

// Shared document operations behind the CLI/server/wasm seam.
//
// The CLI commands (cmd_watermark.c, cmd_metadata.c, cmd_decrypt.c), the web
// server's api_* handlers (server.c), and the wasm shim (wasm/shim.c) used to
// each carry their own copy of the watermark placement math, the metadata
// field-setter loop, and the unlock save-options. The copies drifted — the
// server dropped the CLI's UTF-8->cp1252 re-encoding and the wasm shim ignored
// page /Rotate. These functions are the single implementation for all callers;
// CLI run() bodies and api_* handlers and wasm wrappers are thin adapters over
// them.
//
// Every function here takes an already-open TspdfReader* plus a params struct
// and performs the operation in place. No argv parsing, no HTTP, no printing:
// results are reported through the return value (a TspdfError) and, where the
// caller needs to phrase a specific message, through out-params.
//
// Prefix: tsops_ (internal; deliberately NOT tspdf_ so these symbols stay
// out of the public export filter in scripts/libtspdf.map/.exp).

#include <stdint.h>

#include "../../include/tspdf.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Text watermark ──────────────────────────────────────────────────────
//
// Draw a diagonal text watermark centered on every page, compensating any
// page-level /Rotate so the text reads upright as viewed. `text` is UTF-8 and
// is re-encoded to WinAnsiEncoding (cp1252) for the built-in Helvetica font
// before drawing.

// Extra detail when tsops_watermark_text() returns an encoding error, so the
// caller can name the offending character. Only the field for the returned
// error is meaningful.
typedef struct {
    // TSPDF_ERR_UNSUPPORTED: a code point in `text` had no cp1252 slot; this is
    // that code point (Unicode scalar value).
    uint32_t bad_codepoint;
} TsopsWatermarkTextDetail;

typedef struct {
    const char *text;     // UTF-8 watermark string (required)
    double opacity;       // fill/stroke alpha, 0 < opacity <= 1
    double font_size;     // Helvetica size in points (e.g. 48)
} TsopsWatermarkText;

// Apply a text watermark to `doc` in place. Returns:
//   TSPDF_OK               drawn on all pages, doc modified in place.
//   TSPDF_ERR_ENCODING     `text` is not valid UTF-8.
//   TSPDF_ERR_UNSUPPORTED  a character has no cp1252 slot; the code point is
//                          stored in detail->bad_codepoint (if detail != NULL).
//   TSPDF_ERR_ALLOC        allocation failed.
//   other TspdfError       a per-page overlay/content-stream failure.
// The document is left in a partially-modified state on a mid-run failure;
// callers discard it rather than saving.
TspdfError tsops_watermark_text(TspdfReader *doc,
                                const TsopsWatermarkText *params,
                                TsopsWatermarkTextDetail *detail);

// ── Metadata field setter ───────────────────────────────────────────────
//
// Set one Info-dictionary field by name. `value == NULL` clears the field (the
// serializer then omits it from the rebuilt Info dict). Returns false if `key`
// (its first `key_len` bytes) is not one of the supported keys: title, author,
// subject, keywords, creator, producer.
bool tsops_metadata_set(TspdfReader *doc, const char *key, size_t key_len,
                        const char *value);

// ── Unlock / decrypt save-options ───────────────────────────────────────
//
// Saves preserve a source document's encryption by default; unlocking is the
// explicit opt-out. Returns the default save options with `decrypt` set, so an
// encrypted input is written out unencrypted.
TspdfSaveOptions tsops_unlock_save_options(void);

#ifdef __cplusplus
}
#endif

#endif
