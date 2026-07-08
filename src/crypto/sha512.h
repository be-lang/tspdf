#ifndef TSPDF_SHA512_H
#define TSPDF_SHA512_H
#include <stdint.h>
#include <stddef.h>

/* SHA-512 and SHA-384 (FIPS 180-4). Both share the SHA-512 compression
 * function over 64-bit words; they differ only in their initial hash values
 * (IVs) and in the output length (SHA-512 emits 64 bytes, SHA-384 emits the
 * first 48 bytes). These are needed by the ISO 32000-2 R=6 (Algorithm 2.B)
 * extended password hash, which selects SHA-256/384/512 per round. */

typedef struct {
    uint64_t state[8];
    uint64_t count_lo; /* low 64 bits of the total byte count */
    uint64_t count_hi; /* high 64 bits (lets the 128-bit length field be exact) */
    uint8_t  buffer[128];
} Sha512;

/* SHA-512: 64-byte digest. */
void sha512_init(Sha512 *ctx);
void sha512_update(Sha512 *ctx, const uint8_t *data, size_t len);
void sha512_final(Sha512 *ctx, uint8_t digest[64]);
void sha512_hash(const uint8_t *data, size_t len, uint8_t digest[64]);

/* SHA-384: 48-byte digest (SHA-512 core with SHA-384 IVs, truncated). */
void sha384_init(Sha512 *ctx);
void sha384_final(Sha512 *ctx, uint8_t digest[48]);
void sha384_hash(const uint8_t *data, size_t len, uint8_t digest[48]);
/* sha384_update is identical to sha512_update; reuse it directly. */

#endif
