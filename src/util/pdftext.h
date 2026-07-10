#ifndef TSPDF_PDFTEXT_H
#define TSPDF_PDFTEXT_H

// Shared text-encoding helpers for PDF output: UTF-8 <-> WinAnsiEncoding
// (cp1252) for content drawn with the built-in (base14) fonts, and UTF-16BE
// text strings with BOM for Info-dictionary metadata and outline titles
// (ISO 32000 §7.9.2.2). Definitions live in pdftext.c (part of LIB_SOURCES,
// so both writer-only binaries and the reader link them).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "buffer.h"
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decode one UTF-8 sequence at the start of `s` (NUL-terminated). Writes the
// code point to *out_cp and returns the number of bytes consumed (1-4), or 0
// if the sequence is malformed (bad lead/continuation byte, overlong form,
// surrogate, or out-of-range code point).
size_t tspdf_utf8_decode(const char *s, uint32_t *out_cp);

// Encode `cp` as UTF-8 into `out` (room for at least 4 bytes, no NUL added).
// Returns the number of bytes written; invalid code points become '?'.
size_t tspdf_utf8_encode(uint32_t cp, char *out);

// Unicode code point -> WinAnsiEncoding (cp1252) byte, or -1 if the code
// point has no WinAnsi slot. Covers the full cp1252 table: ASCII, Latin-1
// (0xA0-0xFF) and the 0x80-0x9F specials (euro, dashes, curly quotes, ...).
int tspdf_cp1252_from_codepoint(uint32_t cp);

// Result codes for tspdf_utf8_to_cp1252.
enum {
    TSPDF_PDFTEXT_OK = 0,
    TSPDF_PDFTEXT_UNMAPPED = 1,  // *bad_cp = first code point outside cp1252
    TSPDF_PDFTEXT_BAD_UTF8 = 2   // input is not valid UTF-8
};

// Strict UTF-8 -> cp1252 conversion of a NUL-terminated string. `out` needs
// room for strlen(utf8)+1 bytes (cp1252 output is never longer than the UTF-8
// input) and may alias `utf8` for in-place conversion. Returns one of the
// TSPDF_PDFTEXT_* codes above; on TSPDF_PDFTEXT_UNMAPPED the offending code
// point is stored in *bad_cp (if non-NULL).
int tspdf_utf8_to_cp1252(const char *utf8, char *out, uint32_t *bad_cp);

// Lossy variant: code points outside cp1252 and malformed UTF-8 bytes become
// '?'. Same buffer contract as above (out may alias utf8). Returns the number
// of substitutions made; the first offending code point is stored in
// *first_bad_cp (if non-NULL and any substitution happened).
size_t tspdf_utf8_to_cp1252_lossy(const char *utf8, char *out, uint32_t *first_bad_cp);

// True if `s` contains only ASCII bytes (< 0x80).
bool tspdf_str_is_ascii(const char *s);

// Write a PDF text string for an Info dictionary value: pure-ASCII values are
// written as an escaped literal string; values with non-ASCII UTF-8 become a
// BOM-prefixed UTF-16BE hex string (<FEFF...>) per ISO 32000 §7.9.2.2, so
// other readers do not misread the bytes as PDFDocEncoding. Byte strings that
// are not valid UTF-8 fall back to the escaped-literal form unchanged.
void tspdf_pdftext_write_info_string(TspdfBuffer *buf, const char *value);

// If (data,len) is a UTF-16BE text string with BOM (FE FF), decode it to a
// NUL-terminated UTF-8 string allocated from `arena` and return it; returns
// NULL when there is no BOM (caller keeps the raw bytes) or on alloc failure.
char *tspdf_utf16be_to_utf8(const uint8_t *data, size_t len, TspdfArena *arena);

// PDFDocEncoding byte -> Unicode code point (ISO 32000 Annex D). ASCII and
// the Latin-1 rows map to themselves; the specials in 0x18-0x1F (accents)
// and 0x80-0xA0 (typography, ligatures, euro) map per the table; undefined
// slots (0x7F, 0x9F, 0xAD) become U+FFFD.
uint32_t tspdf_pdfdoc_to_codepoint(uint8_t b);

// Decode a PDF text string (ISO 32000 §7.9.2.2) to a NUL-terminated UTF-8
// string allocated from `arena`:
//   - FE FF BOM        -> UTF-16BE (lone surrogates become '?')
//   - EF BB BF BOM     -> UTF-8 (malformed sequences become U+FFFD)
//   - no BOM           -> kept as-is when the bytes already form valid UTF-8
//                         (pure ASCII, or a non-conformant UTF-8 writer),
//                         otherwise decoded as PDFDocEncoding
// The result is always valid UTF-8. Returns NULL only on alloc failure.
char *tspdf_pdf_text_to_utf8(const uint8_t *data, size_t len, TspdfArena *arena);

// Encode a NUL-terminated UTF-8 string as PDF text-string bytes allocated
// from `arena`: pure ASCII is copied verbatim; other valid UTF-8 becomes
// UTF-16BE with a FE FF BOM (astral code points as surrogate pairs). Byte
// strings that are not valid UTF-8 are copied verbatim so nothing is
// reinterpreted. *out_len receives the byte length (the buffer is also
// NUL-terminated, but UTF-16BE output contains interior NUL bytes — always
// use *out_len). Returns NULL only on alloc failure.
uint8_t *tspdf_utf8_to_pdf_text(const char *utf8, size_t *out_len, TspdfArena *arena);

#ifdef __cplusplus
}
#endif

#endif
