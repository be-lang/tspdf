#ifndef TSPDF_FILTERS_H
#define TSPDF_FILTERS_H

#include <stdint.h>
#include <stddef.h>

#include "../tspdf_error.h"
#include "../reader/tspr.h"   // public TspdfObj/TspdfDictEntry structs only

// Stream-filter decode chain, extracted verbatim from the xref module.
//
// The reader owns raw-byte fetching and crypt decryption ordering; it hands the
// already-decrypted raw stream bytes plus the stream dictionary to this module,
// which applies the /Filter chain (name or array) with per-filter /DecodeParms
// and the PNG/TIFF predictor post-processing.
//
// Dependency direction is reader -> filters: this header pulls in only the
// public reader header (src/reader/tspr.h) for the TspdfObj object model. It
// never includes tspr_internal.h, and walks parameter dicts through the public
// TspdfObj / TspdfDictEntry fields.

// Decode one filter application by (unabbreviated or abbreviated) filter name.
// `params` is the resolved, direct /DecodeParms dict for this filter, or NULL.
// Returns malloc'd bytes (caller frees) with *out_len set and *err = TSPDF_OK,
// or NULL with *err set on failure. Unsupported filters yield
// TSPDF_ERR_UNSUPPORTED.
uint8_t *tspdf_filter_decode(const char *filter_name,
                             const uint8_t *data, size_t len,
                             TspdfObj *params, size_t *out_len,
                             TspdfError *err);

// Apply the full /Filter chain (name or array) plus /DecodeParms taken from the
// stream dictionary to `raw_data`, preserving the existing output caps. Returns
// TSPDF_OK and sets *out_data (malloc'd, caller frees) / *out_len on success.
TspdfError tspdf_filter_decode_chain(TspdfObj *stream_dict,
                                     const uint8_t *raw_data, size_t raw_len,
                                     uint8_t **out_data, size_t *out_len);

#endif // TSPDF_FILTERS_H
