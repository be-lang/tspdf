#ifndef TSPDF_MD5_H
#define TSPDF_MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t buffer[64];
} Md5;

void md5_init(Md5 *ctx);
void md5_update(Md5 *ctx, const uint8_t *data, size_t len);
void md5_final(Md5 *ctx, uint8_t digest[16]);
void md5_hash(const uint8_t *data, size_t len, uint8_t digest[16]);

#endif
