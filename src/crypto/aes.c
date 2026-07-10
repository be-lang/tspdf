#include "aes.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Hardware AES is compiled only under GCC/Clang, for x86 (AES-NI) and
 * AArch64 (ARMv8 crypto extensions): the intrinsics are enabled per-function
 * with __attribute__((target(...))), so the build needs no -maes/-march flag
 * and every other target (wasm/emcc, MSVC, other CPUs) gets the portable
 * path alone. */
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#define TSPDF_AES_HW 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define TSPDF_AES_HW 1
#endif

#ifdef TSPDF_AES_HW
static int aes_hw_available(void);
static void aes_hw_encrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len);
static void aes_hw_decrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len);
#endif

/* -------------------------------------------------------------------------
 * AES (FIPS 197) — ECB and CBC modes, 128-bit and 256-bit keys
 *
 * The block functions are table-driven ("T-tables"): each round of
 * SubBytes + ShiftRows + MixColumns collapses into sixteen 32-bit table
 * lookups XORed with the round key. Decryption runs the equivalent inverse
 * cipher (FIPS 197 §5.3.5) over the dec_keys schedule, so it has the same
 * shape as encryption.
 *
 * Side-channel note: T-table lookups are indexed by key-dependent state
 * bytes, so a co-resident attacker who can prime/probe the data cache could
 * in principle recover key material. The previous byte-oriented code was
 * not constant-time either (S-box lookups, branchy xtime). tspdf is a local
 * file tool, not a network service handling attacker-timed traffic, so
 * cache-timing adversaries are out of its threat model — see
 * docs/known-limitations.md.
 * ------------------------------------------------------------------------- */

/* AES S-box */
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* AES inverse S-box */
static const uint8_t inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Round constants for key expansion */
static const uint8_t rcon[11] = {
    0x00, /* unused */
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

/* -------------------------------------------------------------------------
 * T-tables
 *
 * Te0[x] is the MixColumns column ({02}·S(x), S(x), S(x), {03}·S(x)) as a
 * big-endian word; Te1..Te3 are byte rotations of it, so a full round is
 * four lookups per output word. Td0..Td3 are the InvMixColumns equivalents
 * over the inverse S-box. Generated once from sbox/inv_sbox at the first
 * aes_init (a few microseconds) instead of 8KB of pasted literals.
 *
 * The init is idempotent — every writer stores identical values and
 * tables_ready flips only after they are in place — so the unsynchronized
 * check is a benign race in the library's single-threaded posture.
 * ------------------------------------------------------------------------- */
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static uint32_t Td0[256], Td1[256], Td2[256], Td3[256];
static bool tables_ready = false;

/* GF(2^8) doubling with irreducible poly 0x11b; used only for table setup */
static inline uint8_t xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

static void gen_tables(void) {
    for (int x = 0; x < 256; x++) {
        uint8_t s  = sbox[x];
        uint8_t s2 = xtime(s);
        uint32_t te = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) |
                      ((uint32_t)s  <<  8) |  (uint32_t)(s2 ^ s);
        Te0[x] = te;
        Te1[x] = (te >>  8) | (te << 24);
        Te2[x] = (te >> 16) | (te << 16);
        Te3[x] = (te >> 24) | (te <<  8);

        uint8_t si = inv_sbox[x];
        uint8_t i2 = xtime(si), i4 = xtime(i2), i8 = xtime(i4);
        uint8_t i9 = (uint8_t)(i8 ^ si);            /* {09}·si */
        uint8_t ib = (uint8_t)(i9 ^ i2);            /* {0b}·si */
        uint8_t id = (uint8_t)(i9 ^ i4);            /* {0d}·si */
        uint8_t ie = (uint8_t)(i8 ^ i4 ^ i2);       /* {0e}·si */
        uint32_t td = ((uint32_t)ie << 24) | ((uint32_t)i9 << 16) |
                      ((uint32_t)id <<  8) |  (uint32_t)ib;
        Td0[x] = td;
        Td1[x] = (td >>  8) | (td << 24);
        Td2[x] = (td >> 16) | (td << 16);
        Td3[x] = (td >> 24) | (td <<  8);
    }
    tables_ready = true;
}

/* -------------------------------------------------------------------------
 * Key expansion helpers
 * ------------------------------------------------------------------------- */
static inline uint32_t subword(uint32_t w) {
    return ((uint32_t)sbox[(w >> 24) & 0xff] << 24) |
           ((uint32_t)sbox[(w >> 16) & 0xff] << 16) |
           ((uint32_t)sbox[(w >>  8) & 0xff] <<  8) |
           ((uint32_t)sbox[(w      ) & 0xff]      );
}

static inline uint32_t rotword(uint32_t w) {
    return (w << 8) | (w >> 24);
}

/* InvMixColumns applied to one round-key word (big-endian column), via the
 * Td tables: Td0[sbox[b]] is InvMixColumns of ({b},0,0,0). Used to build the
 * equivalent-inverse-cipher schedule. */
static inline uint32_t inv_mix_word(uint32_t w) {
    return Td0[sbox[(w >> 24) & 0xff]] ^ Td1[sbox[(w >> 16) & 0xff]] ^
           Td2[sbox[(w >>  8) & 0xff]] ^ Td3[sbox[(w      ) & 0xff]];
}

/* -------------------------------------------------------------------------
 * Block load/store — the state lives in four big-endian words s0..s3, one
 * per column
 * ------------------------------------------------------------------------- */
static inline uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static inline void store_be32(uint8_t *p, uint32_t w) {
    p[0] = (uint8_t)(w >> 24);
    p[1] = (uint8_t)(w >> 16);
    p[2] = (uint8_t)(w >>  8);
    p[3] = (uint8_t)(w      );
}

/* -------------------------------------------------------------------------
 * aes_init — key expansion (FIPS 197 §5.2)
 * ------------------------------------------------------------------------- */
void aes_init(Aes *ctx, const uint8_t *key, int key_bits) {
    if (!tables_ready)
        gen_tables();

    int nk = key_bits / 32;          /* words in key: 4 (AES-128) or 8 (AES-256) */
    ctx->nr = nk + 6;                /* rounds: 10 or 14 */
    int total = 4 * (ctx->nr + 1);   /* total words needed */

    /* copy key into first nk words */
    for (int i = 0; i < nk; i++) {
        ctx->round_keys[i] = ((uint32_t)key[4*i    ] << 24) |
                             ((uint32_t)key[4*i + 1] << 16) |
                             ((uint32_t)key[4*i + 2] <<  8) |
                             ((uint32_t)key[4*i + 3]      );
    }

    for (int i = nk; i < total; i++) {
        uint32_t temp = ctx->round_keys[i - 1];
        if (i % nk == 0) {
            temp = subword(rotword(temp)) ^ ((uint32_t)rcon[i / nk] << 24);
        } else if (nk > 6 && i % nk == 4) {
            temp = subword(temp);
        }
        ctx->round_keys[i] = ctx->round_keys[i - nk] ^ temp;
    }

    /* Decrypt schedule for the equivalent inverse cipher (FIPS 197 §5.3.5):
     * encrypt round keys in reverse round order, with InvMixColumns applied
     * to every inner round key. Stored forward so a decryptor walks it the
     * same way the encryptor walks round_keys. */
    for (int i = 0; i <= ctx->nr; i++)
        for (int j = 0; j < 4; j++)
            ctx->dec_keys[4*i + j] = ctx->round_keys[4*(ctx->nr - i) + j];
    for (int i = 1; i < ctx->nr; i++)
        for (int j = 0; j < 4; j++)
            ctx->dec_keys[4*i + j] = inv_mix_word(ctx->dec_keys[4*i + j]);

    /* Hardware dispatch. The hardware round keys are the FIPS schedules
     * above serialized in byte order (each word stored big-endian): AESENC
     * (x86) and AESE (ARM) consume round_keys as-is, and AESDEC / AESD
     * consume the equivalent-inverse-cipher dec_keys, whose per-word
     * InvMixColumns pass is exactly what AESIMC would compute. Deriving both
     * from the one soft expansion keeps the paths on provably identical
     * keys. */
    ctx->use_hw = 0;
#ifdef TSPDF_AES_HW
    if (aes_hw_available()) {
        ctx->use_hw = 1;
        for (int i = 0; i < total; i++) {
            store_be32(ctx->hw_keys     + 4*i, ctx->round_keys[i]);
            store_be32(ctx->hw_dec_keys + 4*i, ctx->dec_keys[i]);
        }
    }
#endif
}

/* -------------------------------------------------------------------------
 * aes_encrypt_ecb — single 16-byte block (FIPS 197 §5.1), T-table rounds.
 * The ECB entry points stay on the soft path: they are used for single
 * blocks (the /Perms entry, key derivation), where dispatch buys nothing.
 * ------------------------------------------------------------------------- */
void aes_encrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *rk = ctx->round_keys;
    uint32_t s0 = load_be32(in     ) ^ rk[0];
    uint32_t s1 = load_be32(in +  4) ^ rk[1];
    uint32_t s2 = load_be32(in +  8) ^ rk[2];
    uint32_t s3 = load_be32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;

    int r = ctx->nr - 1;
    rk += 4;
    for (;;) {
        t0 = Te0[s0 >> 24] ^ Te1[(s1 >> 16) & 0xff] ^ Te2[(s2 >> 8) & 0xff] ^ Te3[s3 & 0xff] ^ rk[0];
        t1 = Te0[s1 >> 24] ^ Te1[(s2 >> 16) & 0xff] ^ Te2[(s3 >> 8) & 0xff] ^ Te3[s0 & 0xff] ^ rk[1];
        t2 = Te0[s2 >> 24] ^ Te1[(s3 >> 16) & 0xff] ^ Te2[(s0 >> 8) & 0xff] ^ Te3[s1 & 0xff] ^ rk[2];
        t3 = Te0[s3 >> 24] ^ Te1[(s0 >> 16) & 0xff] ^ Te2[(s1 >> 8) & 0xff] ^ Te3[s2 & 0xff] ^ rk[3];
        rk += 4;
        if (--r == 0) break;
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }

    /* final round — SubBytes + ShiftRows + AddRoundKey, no MixColumns */
    s0 = ((uint32_t)sbox[t0 >> 24] << 24) | ((uint32_t)sbox[(t1 >> 16) & 0xff] << 16)
       | ((uint32_t)sbox[(t2 >> 8) & 0xff] << 8) | sbox[t3 & 0xff];
    s1 = ((uint32_t)sbox[t1 >> 24] << 24) | ((uint32_t)sbox[(t2 >> 16) & 0xff] << 16)
       | ((uint32_t)sbox[(t3 >> 8) & 0xff] << 8) | sbox[t0 & 0xff];
    s2 = ((uint32_t)sbox[t2 >> 24] << 24) | ((uint32_t)sbox[(t3 >> 16) & 0xff] << 16)
       | ((uint32_t)sbox[(t0 >> 8) & 0xff] << 8) | sbox[t1 & 0xff];
    s3 = ((uint32_t)sbox[t3 >> 24] << 24) | ((uint32_t)sbox[(t0 >> 16) & 0xff] << 16)
       | ((uint32_t)sbox[(t1 >> 8) & 0xff] << 8) | sbox[t2 & 0xff];
    store_be32(out     , s0 ^ rk[0]);
    store_be32(out +  4, s1 ^ rk[1]);
    store_be32(out +  8, s2 ^ rk[2]);
    store_be32(out + 12, s3 ^ rk[3]);
}

/* -------------------------------------------------------------------------
 * aes_decrypt_ecb — single 16-byte block, equivalent inverse cipher
 * (FIPS 197 §5.3.5): same round structure as the forward cipher, consuming
 * the dedicated dec_keys schedule front to back.
 * ------------------------------------------------------------------------- */
void aes_decrypt_ecb(Aes *ctx, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *rk = ctx->dec_keys;
    uint32_t s0 = load_be32(in     ) ^ rk[0];
    uint32_t s1 = load_be32(in +  4) ^ rk[1];
    uint32_t s2 = load_be32(in +  8) ^ rk[2];
    uint32_t s3 = load_be32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;

    int r = ctx->nr - 1;
    rk += 4;
    for (;;) {
        t0 = Td0[s0 >> 24] ^ Td1[(s3 >> 16) & 0xff] ^ Td2[(s2 >> 8) & 0xff] ^ Td3[s1 & 0xff] ^ rk[0];
        t1 = Td0[s1 >> 24] ^ Td1[(s0 >> 16) & 0xff] ^ Td2[(s3 >> 8) & 0xff] ^ Td3[s2 & 0xff] ^ rk[1];
        t2 = Td0[s2 >> 24] ^ Td1[(s1 >> 16) & 0xff] ^ Td2[(s0 >> 8) & 0xff] ^ Td3[s3 & 0xff] ^ rk[2];
        t3 = Td0[s3 >> 24] ^ Td1[(s2 >> 16) & 0xff] ^ Td2[(s1 >> 8) & 0xff] ^ Td3[s0 & 0xff] ^ rk[3];
        rk += 4;
        if (--r == 0) break;
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }

    /* final round — InvSubBytes + InvShiftRows + AddRoundKey, no InvMixColumns */
    s0 = ((uint32_t)inv_sbox[t0 >> 24] << 24) | ((uint32_t)inv_sbox[(t3 >> 16) & 0xff] << 16)
       | ((uint32_t)inv_sbox[(t2 >> 8) & 0xff] << 8) | inv_sbox[t1 & 0xff];
    s1 = ((uint32_t)inv_sbox[t1 >> 24] << 24) | ((uint32_t)inv_sbox[(t0 >> 16) & 0xff] << 16)
       | ((uint32_t)inv_sbox[(t3 >> 8) & 0xff] << 8) | inv_sbox[t2 & 0xff];
    s2 = ((uint32_t)inv_sbox[t2 >> 24] << 24) | ((uint32_t)inv_sbox[(t1 >> 16) & 0xff] << 16)
       | ((uint32_t)inv_sbox[(t0 >> 8) & 0xff] << 8) | inv_sbox[t3 & 0xff];
    s3 = ((uint32_t)inv_sbox[t3 >> 24] << 24) | ((uint32_t)inv_sbox[(t2 >> 16) & 0xff] << 16)
       | ((uint32_t)inv_sbox[(t1 >> 8) & 0xff] << 8) | inv_sbox[t0 & 0xff];
    store_be32(out     , s0 ^ rk[0]);
    store_be32(out +  4, s1 ^ rk[1]);
    store_be32(out +  8, s2 ^ rk[2]);
    store_be32(out + 12, s3 ^ rk[3]);
}

/* -------------------------------------------------------------------------
 * aes_encrypt_cbc — len is truncated to a multiple of 16; in-place (in ==
 * out) is supported.
 * ------------------------------------------------------------------------- */
void aes_encrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len) {
    len &= ~(size_t)15;
#ifdef TSPDF_AES_HW
    if (ctx->use_hw) {
        aes_hw_encrypt_cbc(ctx, iv, in, out, len);
        return;
    }
#endif
    const uint8_t *prev = iv;

    for (size_t i = 0; i < len; i += 16) {
        uint8_t block[16];
        for (int j = 0; j < 16; j++)
            block[j] = in[i + j] ^ prev[j];
        aes_encrypt_ecb(ctx, block, out + i);
        prev = out + i;
    }
}

/* -------------------------------------------------------------------------
 * aes_decrypt_cbc — len is truncated to a multiple of 16; in-place (in ==
 * out) is supported: the ciphertext block is saved before the output write
 * so the soft path matches the hw path's register-held chaining.
 * ------------------------------------------------------------------------- */
void aes_decrypt_cbc(Aes *ctx, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len) {
    len &= ~(size_t)15;
#ifdef TSPDF_AES_HW
    if (ctx->use_hw) {
        aes_hw_decrypt_cbc(ctx, iv, in, out, len);
        return;
    }
#endif
    uint8_t prev[16];
    memcpy(prev, iv, 16);

    for (size_t i = 0; i < len; i += 16) {
        uint8_t saved[16];
        uint8_t block[16];
        memcpy(saved, in + i, 16);
        aes_decrypt_ecb(ctx, in + i, block);
        for (int j = 0; j < 16; j++)
            out[i + j] = block[j] ^ prev[j];
        memcpy(prev, saved, 16);
    }
}

/* =========================================================================
 * Hardware AES
 *
 * Kept at the bottom of this file (not a separate source) so the
 * amalgamation script's hardcoded file list stays valid. Each function
 * carries a per-function __attribute__((target(...))), which lets GCC/Clang
 * emit the AES instructions without -maes/-march on the command line;
 * aes_init only sets use_hw after aes_hw_available() confirms the CPU
 * executes them. Round keys come from the Aes struct's hw_keys/hw_dec_keys
 * byte schedules (see aes_init) via unaligned loads, so no alignment
 * demands leak into the public struct.
 * ========================================================================= */
#ifdef TSPDF_AES_HW
#if defined(__x86_64__) || defined(__i386__)

/* ---- x86 AES-NI ---- */

#include <emmintrin.h>   /* SSE2 */
#include <wmmintrin.h>   /* AES-NI */

/* CPU support probed once; the TSPDF_NO_AESHW override is read per init so
 * tests can flip between paths with setenv + aes_init in one process. */
static int aes_hw_available(void) {
    static int cpu_has_aes = -1;
    if (cpu_has_aes < 0)
        cpu_has_aes = __builtin_cpu_supports("aes") ? 1 : 0;
    const char *off = getenv("TSPDF_NO_AESHW");
    if (off && off[0] != '\0')
        return 0;
    return cpu_has_aes;
}

/* CBC encryption is inherently serial (each block chains into the next), so
 * one block per AESENC chain is the best available shape. */
__attribute__((target("aes,sse2")))
static void aes_hw_encrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len) {
    const int nr = ctx->nr;
    __m128i rk[15];
    for (int r = 0; r <= nr; r++)
        rk[r] = _mm_loadu_si128((const __m128i *)(ctx->hw_keys + 16*r));

    __m128i prev = _mm_loadu_si128((const __m128i *)iv);
    for (size_t i = 0; i < len; i += 16) {
        __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(in + i)), prev);
        x = _mm_xor_si128(x, rk[0]);
        for (int r = 1; r < nr; r++)
            x = _mm_aesenc_si128(x, rk[r]);
        x = _mm_aesenclast_si128(x, rk[nr]);
        _mm_storeu_si128((__m128i *)(out + i), x);
        prev = x;
    }
}

/* CBC decryption is parallel (every block's cipher input is ciphertext we
 * already hold), so run four AESDEC chains at once to cover the instruction
 * latency, then XOR each result with the preceding ciphertext block. */
__attribute__((target("aes,sse2")))
static void aes_hw_decrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len) {
    const int nr = ctx->nr;
    __m128i rk[15];
    for (int r = 0; r <= nr; r++)
        rk[r] = _mm_loadu_si128((const __m128i *)(ctx->hw_dec_keys + 16*r));

    __m128i prev = _mm_loadu_si128((const __m128i *)iv);
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        __m128i c0 = _mm_loadu_si128((const __m128i *)(in + i));
        __m128i c1 = _mm_loadu_si128((const __m128i *)(in + i + 16));
        __m128i c2 = _mm_loadu_si128((const __m128i *)(in + i + 32));
        __m128i c3 = _mm_loadu_si128((const __m128i *)(in + i + 48));
        __m128i x0 = _mm_xor_si128(c0, rk[0]);
        __m128i x1 = _mm_xor_si128(c1, rk[0]);
        __m128i x2 = _mm_xor_si128(c2, rk[0]);
        __m128i x3 = _mm_xor_si128(c3, rk[0]);
        for (int r = 1; r < nr; r++) {
            x0 = _mm_aesdec_si128(x0, rk[r]);
            x1 = _mm_aesdec_si128(x1, rk[r]);
            x2 = _mm_aesdec_si128(x2, rk[r]);
            x3 = _mm_aesdec_si128(x3, rk[r]);
        }
        x0 = _mm_aesdeclast_si128(x0, rk[nr]);
        x1 = _mm_aesdeclast_si128(x1, rk[nr]);
        x2 = _mm_aesdeclast_si128(x2, rk[nr]);
        x3 = _mm_aesdeclast_si128(x3, rk[nr]);
        _mm_storeu_si128((__m128i *)(out + i),      _mm_xor_si128(x0, prev));
        _mm_storeu_si128((__m128i *)(out + i + 16), _mm_xor_si128(x1, c0));
        _mm_storeu_si128((__m128i *)(out + i + 32), _mm_xor_si128(x2, c1));
        _mm_storeu_si128((__m128i *)(out + i + 48), _mm_xor_si128(x3, c2));
        prev = c3;
    }
    for (; i < len; i += 16) {
        __m128i ct = _mm_loadu_si128((const __m128i *)(in + i));
        __m128i x = _mm_xor_si128(ct, rk[0]);
        for (int r = 1; r < nr; r++)
            x = _mm_aesdec_si128(x, rk[r]);
        x = _mm_aesdeclast_si128(x, rk[nr]);
        _mm_storeu_si128((__m128i *)(out + i), _mm_xor_si128(x, prev));
        prev = ct;
    }
}

#else /* __aarch64__ */

/* ---- ARMv8 crypto extensions (AArch64) ----
 *
 * The NEON AES instructions cut the round at a different joint than AES-NI:
 * AESE (vaeseq_u8) computes ShiftRows(SubBytes(state ^ key)) — AddRoundKey
 * happens FIRST, inside the instruction — and MixColumns is the separate
 * AESMC (vaesmcq_u8). Folding the FIPS cipher
 *
 *     state = pt ^ rk[0]
 *     rounds 1..Nr-1:  state = MixColumns(ShiftRows(SubBytes(state))) ^ rk[r]
 *     final:           state = ShiftRows(SubBytes(state)) ^ rk[Nr]
 *
 * into that shape absorbs each round's trailing AddRoundKey into the next
 * AESE's leading key XOR:
 *
 *     for r = 0..Nr-2:  state = AESMC(AESE(state, rk[r]))
 *     state = AESE(state, rk[Nr-1]) ^ rk[Nr]
 *
 * so encryption consumes the same hw_keys schedule as AESENC, front to
 * back: AESE/AESD act bytewise on the state's plain memory layout, which is
 * exactly the big-endian-per-word serialization hw_keys holds, so vld1q_u8
 * loads the keys directly. Decryption mirrors this with AESD (vaesdq_u8,
 * key XOR then InvSubBytes/InvShiftRows) and AESIMC (vaesimcq_u8). The
 * equivalent inverse cipher (see aes_init: dec_keys[0] = rk[Nr], middle
 * keys InvMixColumns'd, dec_keys[Nr] = rk[0]) folds the same way —
 *
 *     for r = 0..Nr-2:  state = AESIMC(AESD(state, dk[r]))
 *     state = AESD(state, dk[Nr-1]) ^ dk[Nr]
 *
 * — because each round's "^ dk[r] then InvMixColumns of the state" in the
 * software path splits into AESIMC now and the key XOR absorbed by the next
 * AESD. hw_dec_keys is therefore consumed as stored, in the same order
 * AESDEC uses it. */

#include <arm_neon.h>

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1UL << 3)   /* Linux UAPI bit; missing from older headers */
#endif
#endif

/* CPU support probed once; the TSPDF_NO_AESHW override is read per init so
 * tests can flip between paths with setenv + aes_init in one process. */
static int aes_hw_available(void) {
#if defined(__APPLE__)
    /* AES is architecturally guaranteed here: every Apple Silicon CPU
     * (M1 onward) implements FEAT_AES, and arm64 macOS runs on nothing
     * else, so there is no runtime probe to do. */
    int cpu_has_aes = 1;
#elif defined(__linux__)
    static int cpu_has_aes = -1;
    if (cpu_has_aes < 0)
        cpu_has_aes = (getauxval(AT_HWCAP) & HWCAP_AES) ? 1 : 0;
#else
    /* No portable probe on this OS — stay on the C path. */
    int cpu_has_aes = 0;
#endif
    const char *off = getenv("TSPDF_NO_AESHW");
    if (off && off[0] != '\0')
        return 0;
    return cpu_has_aes;
}

/* CBC encryption is inherently serial (each block chains into the next), so
 * one block per AESE/AESMC chain is the best available shape. */
__attribute__((target("+crypto")))
static void aes_hw_encrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len) {
    const int nr = ctx->nr;
    uint8x16_t rk[15];
    for (int r = 0; r <= nr; r++)
        rk[r] = vld1q_u8(ctx->hw_keys + 16*r);

    uint8x16_t prev = vld1q_u8(iv);
    for (size_t i = 0; i < len; i += 16) {
        uint8x16_t x = veorq_u8(vld1q_u8(in + i), prev);
        for (int r = 0; r < nr - 1; r++)
            x = vaesmcq_u8(vaeseq_u8(x, rk[r]));
        x = veorq_u8(vaeseq_u8(x, rk[nr - 1]), rk[nr]);
        vst1q_u8(out + i, x);
        prev = x;
    }
}

/* CBC decryption is parallel (every block's cipher input is ciphertext we
 * already hold), so run four AESD chains at once to cover the instruction
 * latency, then XOR each result with the preceding ciphertext block. */
__attribute__((target("+crypto")))
static void aes_hw_decrypt_cbc(const Aes *ctx, const uint8_t iv[16],
                               const uint8_t *in, uint8_t *out, size_t len) {
    const int nr = ctx->nr;
    uint8x16_t rk[15];
    for (int r = 0; r <= nr; r++)
        rk[r] = vld1q_u8(ctx->hw_dec_keys + 16*r);

    uint8x16_t prev = vld1q_u8(iv);
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        uint8x16_t c0 = vld1q_u8(in + i);
        uint8x16_t c1 = vld1q_u8(in + i + 16);
        uint8x16_t c2 = vld1q_u8(in + i + 32);
        uint8x16_t c3 = vld1q_u8(in + i + 48);
        uint8x16_t x0 = c0, x1 = c1, x2 = c2, x3 = c3;
        for (int r = 0; r < nr - 1; r++) {
            x0 = vaesimcq_u8(vaesdq_u8(x0, rk[r]));
            x1 = vaesimcq_u8(vaesdq_u8(x1, rk[r]));
            x2 = vaesimcq_u8(vaesdq_u8(x2, rk[r]));
            x3 = vaesimcq_u8(vaesdq_u8(x3, rk[r]));
        }
        x0 = veorq_u8(vaesdq_u8(x0, rk[nr - 1]), rk[nr]);
        x1 = veorq_u8(vaesdq_u8(x1, rk[nr - 1]), rk[nr]);
        x2 = veorq_u8(vaesdq_u8(x2, rk[nr - 1]), rk[nr]);
        x3 = veorq_u8(vaesdq_u8(x3, rk[nr - 1]), rk[nr]);
        vst1q_u8(out + i,      veorq_u8(x0, prev));
        vst1q_u8(out + i + 16, veorq_u8(x1, c0));
        vst1q_u8(out + i + 32, veorq_u8(x2, c1));
        vst1q_u8(out + i + 48, veorq_u8(x3, c2));
        prev = c3;
    }
    for (; i < len; i += 16) {
        uint8x16_t ct = vld1q_u8(in + i);
        uint8x16_t x = ct;
        for (int r = 0; r < nr - 1; r++)
            x = vaesimcq_u8(vaesdq_u8(x, rk[r]));
        x = veorq_u8(vaesdq_u8(x, rk[nr - 1]), rk[nr]);
        vst1q_u8(out + i, veorq_u8(x, prev));
        prev = ct;
    }
}

#endif /* arch */
#endif /* TSPDF_AES_HW */
