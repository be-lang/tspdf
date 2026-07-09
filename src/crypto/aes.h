#ifndef TSPDF_AES_H
#define TSPDF_AES_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t round_keys[60];    // encrypt schedule; max 14 rounds * 4 + 4 = 60 words
    uint32_t dec_keys[60];      // decrypt schedule for the equivalent inverse cipher
                                // (FIPS 197 §5.3.5): round keys in reverse order with
                                // InvMixColumns applied to the inner rounds
    int nr;                     // number of rounds (10 or 14)
    int use_hw;                 // 1 = CBC calls dispatch to the hardware path
                                // (x86 AES-NI or ARMv8 crypto extensions); 0
                                // everywhere the hw path is not compiled, the
                                // CPU lacks it, or TSPDF_NO_AESHW was set
    uint8_t hw_keys[240];       // round_keys serialized big-endian per word — the
                                // FIPS byte order the AESENC/AESE round keys use.
                                // Filled only when use_hw.
    uint8_t hw_dec_keys[240];   // dec_keys likewise; the equivalent-inverse-cipher
                                // schedule is exactly what AESDEC and AESD+AESIMC
                                // expect (the InvMixColumns pass equals AESIMC
                                // per round key)
} Aes;

void aes_init(Aes *ctx, const uint8_t *key, int key_bits); // 128 or 256
void aes_encrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]);
void aes_decrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]);
void aes_encrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
void aes_decrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
#endif
