#include "sha512.h"
#include <string.h>

/* Round constants: first 64 bits of the fractional parts of the cube roots of
 * the first 80 primes (FIPS 180-4, section 4.2.3). */
static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define SIGMA0(x) (ROTR(x, 28) ^ ROTR(x, 34) ^ ROTR(x, 39))
#define SIGMA1(x) (ROTR(x, 14) ^ ROTR(x, 18) ^ ROTR(x, 41))
#define sigma0(x) (ROTR(x,  1) ^ ROTR(x,  8) ^ ((x) >>  7))
#define sigma1(x) (ROTR(x, 19) ^ ROTR(x, 61) ^ ((x) >>  6))

static void sha512_transform(Sha512 *ctx, const uint8_t block[128]) {
    uint64_t W[80];
    uint64_t a, b, c, d, e, f, g, h;
    uint64_t T1, T2;
    int i;

    /* Prepare message schedule (big-endian 64-bit words) */
    for (i = 0; i < 16; i++) {
        W[i] = ((uint64_t)block[i*8    ] << 56)
             | ((uint64_t)block[i*8 + 1] << 48)
             | ((uint64_t)block[i*8 + 2] << 40)
             | ((uint64_t)block[i*8 + 3] << 32)
             | ((uint64_t)block[i*8 + 4] << 24)
             | ((uint64_t)block[i*8 + 5] << 16)
             | ((uint64_t)block[i*8 + 6] <<  8)
             | ((uint64_t)block[i*8 + 7]);
    }
    for (i = 16; i < 80; i++) {
        W[i] = sigma1(W[i-2]) + W[i-7] + sigma0(W[i-15]) + W[i-16];
    }

    /* Initialize working variables */
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    /* 80 rounds */
    for (i = 0; i < 80; i++) {
        T1 = h + SIGMA1(e) + CH(e, f, g) + K[i] + W[i];
        T2 = SIGMA0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    /* Add compressed chunk to current hash value */
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha512_init(Sha512 *ctx) {
    /* Initial hash values: first 64 bits of the fractional parts of the square
     * roots of the first 8 primes (FIPS 180-4, section 5.3.5). */
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->count_lo = 0;
    ctx->count_hi = 0;
}

void sha384_init(Sha512 *ctx) {
    /* SHA-384 IVs: first 64 bits of the fractional parts of the square roots of
     * the 9th–16th primes (FIPS 180-4, section 5.3.4). Same compression core. */
    ctx->state[0] = 0xcbbb9d5dc1059ed8ULL;
    ctx->state[1] = 0x629a292a367cd507ULL;
    ctx->state[2] = 0x9159015a3070dd17ULL;
    ctx->state[3] = 0x152fecd8f70e5939ULL;
    ctx->state[4] = 0x67332667ffc00b31ULL;
    ctx->state[5] = 0x8eb44a8768581511ULL;
    ctx->state[6] = 0xdb0c2e0d64f98fa7ULL;
    ctx->state[7] = 0x47b5481dbefa4fa4ULL;
    ctx->count_lo = 0;
    ctx->count_hi = 0;
}

void sha512_update(Sha512 *ctx, const uint8_t *data, size_t len) {
    size_t buf_used = (size_t)(ctx->count_lo & 127);

    /* Maintain a 128-bit byte count so the final length field is exact even for
     * inputs that overflow 64 bits (academic for our use, but matches the spec). */
    uint64_t old_lo = ctx->count_lo;
    ctx->count_lo += len;
    if (ctx->count_lo < old_lo)
        ctx->count_hi++;

    /* Fill partial buffer first */
    if (buf_used > 0) {
        size_t space = 128 - buf_used;
        if (len < space) {
            memcpy(ctx->buffer + buf_used, data, len);
            return;
        }
        memcpy(ctx->buffer + buf_used, data, space);
        sha512_transform(ctx, ctx->buffer);
        data += space;
        len  -= space;
    }

    /* Process full blocks directly from input */
    while (len >= 128) {
        sha512_transform(ctx, data);
        data += 128;
        len  -= 128;
    }

    /* Store remaining bytes */
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

/* Shared finalization: pad, append the 128-bit big-endian bit length, run the
 * last transform(s), then emit `out_words` state words big-endian.
 * SHA-512 emits all 8 (64 bytes); SHA-384 emits the first 6 (48 bytes). */
static void sha512_finalize(Sha512 *ctx, uint8_t *digest, int out_words) {
    /* bit count = byte count * 8, as a 128-bit value */
    uint64_t bit_lo = ctx->count_lo << 3;
    uint64_t bit_hi = (ctx->count_hi << 3) | (ctx->count_lo >> 61);
    size_t buf_used = (size_t)(ctx->count_lo & 127);

    /* Append 0x80 padding byte */
    ctx->buffer[buf_used++] = 0x80;

    /* If not enough room for the 16-byte length, pad to end and transform */
    if (buf_used > 112) {
        memset(ctx->buffer + buf_used, 0, 128 - buf_used);
        sha512_transform(ctx, ctx->buffer);
        buf_used = 0;
    }

    /* Zero-pad to 112 bytes, then append 128-bit big-endian bit count */
    memset(ctx->buffer + buf_used, 0, 112 - buf_used);
    for (int i = 0; i < 8; i++) {
        ctx->buffer[112 + i] = (uint8_t)(bit_hi >> (56 - i*8));
        ctx->buffer[120 + i] = (uint8_t)(bit_lo >> (56 - i*8));
    }
    sha512_transform(ctx, ctx->buffer);

    /* Output digest in big-endian byte order */
    for (int i = 0; i < out_words; i++) {
        digest[i*8    ] = (uint8_t)(ctx->state[i] >> 56);
        digest[i*8 + 1] = (uint8_t)(ctx->state[i] >> 48);
        digest[i*8 + 2] = (uint8_t)(ctx->state[i] >> 40);
        digest[i*8 + 3] = (uint8_t)(ctx->state[i] >> 32);
        digest[i*8 + 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i*8 + 5] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*8 + 6] = (uint8_t)(ctx->state[i] >>  8);
        digest[i*8 + 7] = (uint8_t)(ctx->state[i]      );
    }
}

void sha512_final(Sha512 *ctx, uint8_t digest[64]) {
    sha512_finalize(ctx, digest, 8);
}

void sha384_final(Sha512 *ctx, uint8_t digest[48]) {
    sha512_finalize(ctx, digest, 6);
}

void sha512_hash(const uint8_t *data, size_t len, uint8_t digest[64]) {
    Sha512 ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, digest);
}

void sha384_hash(const uint8_t *data, size_t len, uint8_t digest[48]) {
    Sha512 ctx;
    sha384_init(&ctx);
    sha512_update(&ctx, data, len);
    sha384_final(&ctx, digest);
}
