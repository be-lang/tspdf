#ifndef TSPDF_DEFLATE_H
#define TSPDF_DEFLATE_H

#include <stdint.h>
#include <stddef.h>

// Hard limit on inflate output (decompressed) size per call. Streams that would
// exceed this are rejected to bound memory use (RFC 1951 has no length field).
#define TSPDF_DEFLATE_MAX_OUTPUT (128u * 1024u * 1024u)

// Compress data using deflate (RFC 1951) with zlib wrapper (RFC 1950).
// Returns malloc'd buffer with compressed data, or NULL on failure.
// Sets *out_len to the length of the compressed data.
// Caller must free() the returned buffer.
uint8_t *deflate_compress(const uint8_t *data, size_t len, size_t *out_len);

// Decompress deflate data with zlib wrapper (RFC 1950).
// Returns malloc'd buffer with decompressed data, or NULL on failure.
// Sets *out_len to the length of the decompressed data.
// Caller must free() the returned buffer.
uint8_t *deflate_decompress(const uint8_t *data, size_t len, size_t *out_len);

#endif
