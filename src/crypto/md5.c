#include "md5.h"
#include <string.h>

/* Per-round shift amounts */
static const uint32_t S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

/* Precomputed T[i] = floor(abs(sin(i+1)) * 2^32) */
static const uint32_t T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define F(b, c, d) (((b) & (c)) | (~(b) & (d)))
#define G(b, c, d) (((b) & (d)) | ((c) & ~(d)))
#define H(b, c, d) ((b) ^ (c) ^ (d))
#define I(b, c, d) ((c) ^ ((b) | ~(d)))

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i*4]
             | ((uint32_t)block[i*4+1] << 8)
             | ((uint32_t)block[i*4+2] << 16)
             | ((uint32_t)block[i*4+3] << 24);
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];

    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) {
            f = F(b, c, d);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = G(b, c, d);
            g = (5u * (uint32_t)i + 1u) % 16u;
        } else if (i < 48) {
            f = H(b, c, d);
            g = (3u * (uint32_t)i + 5u) % 16u;
        } else {
            f = I(b, c, d);
            g = (7u * (uint32_t)i) % 16u;
        }
        f = f + a + T[i] + M[g];
        a = d;
        d = c;
        c = b;
        b = b + ROTL32(f, S[i]);
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void md5_init(Md5 *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

void md5_update(Md5 *ctx, const uint8_t *data, size_t len) {
    size_t buf_used = (size_t)(ctx->count % 64);
    ctx->count += len;

    /* Fill partial buffer first */
    if (buf_used > 0) {
        size_t space = 64 - buf_used;
        size_t take = len < space ? len : space;
        memcpy(ctx->buffer + buf_used, data, take);
        data += take;
        len -= take;
        buf_used += take;
        if (buf_used == 64) {
            md5_transform(ctx->state, ctx->buffer);
        }
    }

    /* Process full blocks directly from input */
    while (len >= 64) {
        md5_transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    /* Store remaining bytes in buffer */
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

void md5_final(Md5 *ctx, uint8_t digest[16]) {
    uint64_t bit_count = ctx->count * 8;
    size_t buf_used = (size_t)(ctx->count % 64);

    /* Append 0x80 padding byte */
    ctx->buffer[buf_used++] = 0x80;

    /* If no room for 8-byte length, pad to end and process */
    if (buf_used > 56) {
        memset(ctx->buffer + buf_used, 0, 64 - buf_used);
        md5_transform(ctx->state, ctx->buffer);
        buf_used = 0;
    }

    /* Zero-fill up to 56 bytes */
    memset(ctx->buffer + buf_used, 0, 56 - buf_used);

    /* Append bit count as 64-bit little-endian */
    ctx->buffer[56] = (uint8_t)(bit_count);
    ctx->buffer[57] = (uint8_t)(bit_count >> 8);
    ctx->buffer[58] = (uint8_t)(bit_count >> 16);
    ctx->buffer[59] = (uint8_t)(bit_count >> 24);
    ctx->buffer[60] = (uint8_t)(bit_count >> 32);
    ctx->buffer[61] = (uint8_t)(bit_count >> 40);
    ctx->buffer[62] = (uint8_t)(bit_count >> 48);
    ctx->buffer[63] = (uint8_t)(bit_count >> 56);

    md5_transform(ctx->state, ctx->buffer);

    /* Output digest as 16 bytes little-endian */
    for (int i = 0; i < 4; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i]);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

void md5_hash(const uint8_t *data, size_t len, uint8_t digest[16]) {
    Md5 ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, digest);
}
