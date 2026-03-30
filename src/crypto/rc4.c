#include "rc4.h"

void rc4_init(Rc4 *ctx, const uint8_t *key, size_t key_len) {
    uint8_t *S = ctx->S;
    for (int i = 0; i < 256; i++)
        S[i] = (uint8_t)i;

    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = j + S[i] + key[i % key_len];
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
    }

    ctx->i = 0;
    ctx->j = 0;
}

void rc4_crypt(Rc4 *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t *S = ctx->S;
    uint8_t i = ctx->i;
    uint8_t j = ctx->j;

    for (size_t n = 0; n < len; n++) {
        i = i + 1;
        j = j + S[i];
        uint8_t tmp = S[i]; S[i] = S[j]; S[j] = tmp;
        out[n] = in[n] ^ S[(uint8_t)(S[i] + S[j])];
    }

    ctx->i = i;
    ctx->j = j;
}
