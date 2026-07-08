#ifndef TSPDF_SHA256_H
#define TSPDF_SHA256_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} Sha256;

void sha256_init(Sha256 *ctx);
void sha256_update(Sha256 *ctx, const uint8_t *data, size_t len);
void sha256_final(Sha256 *ctx, uint8_t digest[32]);
void sha256_hash(const uint8_t *data, size_t len, uint8_t digest[32]);
#endif
