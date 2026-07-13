#ifndef TSPDF_PDF_PRIMITIVES_H
#define TSPDF_PDF_PRIMITIVES_H

/*
 * Canonical PDF-syntax primitive encoders.
 *
 * Both the reader serializer (tspr_serialize.c) and the raw writer
 * (pdf_writer.c) delegate here so escaping logic lives in exactly one place.
 *
 * String encoding (ISO 32000 §7.3.4):
 *   ( ) \  -> \( \) \\
 *   \n \r \t \b \f  -> two-char escapes
 *   bytes < 32 or > 126  -> \NNN (three-octal-digit)
 *
 * Name encoding (ISO 32000 §7.3.5):
 *   bytes < 33 or > 126, or any of  # / ( ) < > [ ] { } %  -> #HH
 */

#include "../util/buffer.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encode a PDF literal string into buf.
 * Writes the opening '(' ... closing ')' delimiters.
 * data/len may be arbitrary bytes (not NUL-terminated).
 */
void tspdf_pdf_encode_string(TspdfBuffer *buf, const uint8_t *data, size_t len);

/*
 * Encode a PDF name into buf.
 * Writes the leading '/' followed by the #HH-escaped name body.
 * data/len may be arbitrary bytes (not NUL-terminated).
 */
void tspdf_pdf_encode_name(TspdfBuffer *buf, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TSPDF_PDF_PRIMITIVES_H */
