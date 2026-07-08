#ifndef TSPDF_RC4_H
#define TSPDF_RC4_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t S[256];
    uint8_t i, j;
} Rc4;

void rc4_init(Rc4 *ctx, const uint8_t *key, size_t key_len);
void rc4_crypt(Rc4 *ctx, const uint8_t *in, uint8_t *out, size_t len);
#endif
