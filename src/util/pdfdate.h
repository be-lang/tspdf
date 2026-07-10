#ifndef TSPDF_PDFDATE_H
#define TSPDF_PDFDATE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Format a PDF date string (ISO 32000-1 section 7.9.4, "D:YYYYMMDDHHmmSS"
 * with an optional O HH' mm' zone suffix) as human-readable text:
 *
 *   D:20131031140150+04'00'  ->  2013-10-31 14:01:50 +04:00
 *   D:20131031140150Z        ->  2013-10-31 14:01:50 UTC
 *   D:201310                 ->  2013-10-01 00:00:00
 *
 * The "D:" prefix and every field after the year are optional (missing
 * fields default per the spec: month/day 01, time 00). Returns true and
 * writes a NUL-terminated string into `out` on success; returns false
 * (leaving `out` untouched) when `raw` is not a well-formed PDF date, so
 * callers can fall back to printing the raw value.
 */
bool tspdf_format_pdf_date(const char *raw, char *out, size_t out_size);

#endif /* TSPDF_PDFDATE_H */
