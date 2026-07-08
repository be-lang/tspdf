#ifndef TSPDF_AES_H
#define TSPDF_AES_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t round_keys[60];    // max 14 rounds * 4 + 4 = 60 words
    int nr;                     // number of rounds (10 or 14)
} Aes;

void aes_init(Aes *ctx, const uint8_t *key, int key_bits); // 128 or 256
void aes_encrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]);
void aes_encrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
void aes_decrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
#endif
