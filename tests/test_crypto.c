#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "test_framework.h"
#include "../src/crypto/md5.h"
#include "../src/crypto/sha256.h"
#include "../src/crypto/sha512.h"
#include "../src/crypto/rc4.h"
#include "../src/crypto/aes.h"

static bool digest_eq(const uint8_t *digest, size_t len, const char *hex) {
    for (size_t i = 0; i < len; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", digest[i]);
        if (buf[0] != hex[i*2] || buf[1] != hex[i*2+1]) return false;
    }
    return true;
}

TEST(test_md5_empty) {
    uint8_t d[16];
    md5_hash((const uint8_t *)"", 0, d);
    ASSERT(digest_eq(d, 16, "d41d8cd98f00b204e9800998ecf8427e"));
}

TEST(test_md5_a) {
    uint8_t d[16];
    md5_hash((const uint8_t *)"a", 1, d);
    ASSERT(digest_eq(d, 16, "0cc175b9c0f1b6a831c399e269772661"));
}

TEST(test_md5_abc) {
    uint8_t d[16];
    md5_hash((const uint8_t *)"abc", 3, d);
    ASSERT(digest_eq(d, 16, "900150983cd24fb0d6963f7d28e17f72"));
}

TEST(test_md5_message_digest) {
    uint8_t d[16];
    const char *msg = "message digest";
    md5_hash((const uint8_t *)msg, strlen(msg), d);
    ASSERT(digest_eq(d, 16, "f96b697d7cb7938d525a2f31aaf161d0"));
}

TEST(test_md5_alphabet) {
    uint8_t d[16];
    const char *msg = "abcdefghijklmnopqrstuvwxyz";
    md5_hash((const uint8_t *)msg, strlen(msg), d);
    ASSERT(digest_eq(d, 16, "c3fcd3d76192e4007dfb496cca67e13b"));
}

TEST(test_md5_incremental) {
    Md5 ctx;
    md5_init(&ctx);
    md5_update(&ctx, (const uint8_t *)"ab", 2);
    md5_update(&ctx, (const uint8_t *)"c", 1);
    uint8_t d[16];
    md5_final(&ctx, d);
    ASSERT(digest_eq(d, 16, "900150983cd24fb0d6963f7d28e17f72"));
}

TEST(test_sha256_empty) {
    uint8_t d[32];
    sha256_hash((const uint8_t *)"", 0, d);
    ASSERT(digest_eq(d, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST(test_sha256_abc) {
    uint8_t d[32];
    sha256_hash((const uint8_t *)"abc", 3, d);
    ASSERT(digest_eq(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(test_sha256_long) {
    uint8_t d[32];
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hash((const uint8_t *)msg, strlen(msg), d);
    ASSERT(digest_eq(d, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(test_sha256_incremental) {
    Sha256 ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"ab", 2);
    sha256_update(&ctx, (const uint8_t *)"c", 1);
    uint8_t d[32];
    sha256_final(&ctx, d);
    ASSERT(digest_eq(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

/* SHA-384 / SHA-512 known-answer vectors from FIPS 180-4 and the original
 * Secure Hash Standard examples. These exercise the from-scratch SHA-512 core
 * required by the ISO 32000-2 R=6 (Algorithm 2.B) extended password hash. */

TEST(test_sha512_empty) {
    uint8_t d[64];
    sha512_hash((const uint8_t *)"", 0, d);
    ASSERT(digest_eq(d, 64,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
}

TEST(test_sha512_abc) {
    uint8_t d[64];
    sha512_hash((const uint8_t *)"abc", 3, d);
    ASSERT(digest_eq(d, 64,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
}

TEST(test_sha512_long) {
    /* 112-char message: forces a second padded block (length > 111 bytes). */
    uint8_t d[64];
    const char *msg = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512_hash((const uint8_t *)msg, strlen(msg), d);
    ASSERT(digest_eq(d, 64,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
}

TEST(test_sha512_incremental) {
    Sha512 ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, (const uint8_t *)"ab", 2);
    sha512_update(&ctx, (const uint8_t *)"c", 1);
    uint8_t d[64];
    sha512_final(&ctx, d);
    ASSERT(digest_eq(d, 64,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
}

TEST(test_sha384_empty) {
    uint8_t d[48];
    sha384_hash((const uint8_t *)"", 0, d);
    ASSERT(digest_eq(d, 48,
        "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
        "274edebfe76f65fbd51ad2f14898b95b"));
}

TEST(test_sha384_abc) {
    uint8_t d[48];
    sha384_hash((const uint8_t *)"abc", 3, d);
    ASSERT(digest_eq(d, 48,
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7"));
}

TEST(test_sha384_long) {
    /* Same 112-char message; exercises SHA-384's two-block padding path. */
    uint8_t d[48];
    const char *msg = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha384_hash((const uint8_t *)msg, strlen(msg), d);
    ASSERT(digest_eq(d, 48,
        "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712"
        "fcc7c71a557e2db966c3e9fa91746039"));
}

TEST(test_sha384_incremental) {
    Sha512 ctx;
    sha384_init(&ctx);
    sha512_update(&ctx, (const uint8_t *)"ab", 2);
    sha512_update(&ctx, (const uint8_t *)"c", 1);
    uint8_t d[48];
    sha384_final(&ctx, d);
    ASSERT(digest_eq(d, 48,
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
        "8086072ba1e7cc2358baeca134c825a7"));
}

TEST(test_rc4_basic) {
    const uint8_t key[] = "Key";
    const uint8_t plaintext[] = "Plaintext";
    const uint8_t expected[] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    uint8_t out[9];
    Rc4 ctx;
    rc4_init(&ctx, key, 3);
    rc4_crypt(&ctx, plaintext, out, 9);
    ASSERT(memcmp(out, expected, 9) == 0);
}

TEST(test_rc4_wiki) {
    const uint8_t key[] = "Wiki";
    const uint8_t plaintext[] = "pedia";
    const uint8_t expected[] = {0x10, 0x21, 0xBF, 0x04, 0x20};
    uint8_t out[5];
    Rc4 ctx;
    rc4_init(&ctx, key, 4);
    rc4_crypt(&ctx, plaintext, out, 5);
    ASSERT(memcmp(out, expected, 5) == 0);
}

TEST(test_rc4_roundtrip) {
    const uint8_t key[] = "secret";
    const uint8_t plaintext[] = "Hello, World!";
    uint8_t encrypted[13], decrypted[13];
    Rc4 ctx;
    rc4_init(&ctx, key, 6);
    rc4_crypt(&ctx, plaintext, encrypted, 13);
    rc4_init(&ctx, key, 6);
    rc4_crypt(&ctx, encrypted, decrypted, 13);
    ASSERT(memcmp(decrypted, plaintext, 13) == 0);
}

TEST(test_aes128_ecb) {
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t plaintext[16] = {
        0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,
        0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34
    };
    const uint8_t expected[16] = {
        0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,
        0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32
    };
    Aes ctx;
    aes_init(&ctx, key, 128);
    uint8_t out[16];
    aes_encrypt_ecb(&ctx, plaintext, out);
    ASSERT(memcmp(out, expected, 16) == 0);
    uint8_t dec[16];
    aes_decrypt_ecb(&ctx, out, dec);
    ASSERT(memcmp(dec, plaintext, 16) == 0);
}

TEST(test_aes256_ecb) {
    const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    const uint8_t plaintext[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    const uint8_t expected[16] = {
        0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
        0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89
    };
    Aes ctx;
    aes_init(&ctx, key, 256);
    uint8_t out[16];
    aes_encrypt_ecb(&ctx, plaintext, out);
    ASSERT(memcmp(out, expected, 16) == 0);
    uint8_t dec[16];
    aes_decrypt_ecb(&ctx, out, dec);
    ASSERT(memcmp(dec, plaintext, 16) == 0);
}

/* FIPS-197 appendix C decrypt-only KATs: a freshly initialized context that
 * never ran the encrypt direction must invert the appendix ciphertext. This
 * pins the decrypt key schedule independently of the encrypt schedule. */

TEST(test_aes128_ecb_decrypt_kat) {
    const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    /* FIPS-197 C.1: plaintext 00112233445566778899aabbccddeeff */
    const uint8_t ciphertext[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };
    const uint8_t expected[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    Aes ctx;
    aes_init(&ctx, key, 128);
    uint8_t out[16];
    aes_decrypt_ecb(&ctx, ciphertext, out);
    ASSERT(memcmp(out, expected, 16) == 0);
}

TEST(test_aes256_ecb_decrypt_kat) {
    const uint8_t key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
    };
    /* FIPS-197 C.3: plaintext 00112233445566778899aabbccddeeff */
    const uint8_t ciphertext[16] = {
        0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,
        0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89
    };
    const uint8_t expected[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    Aes ctx;
    aes_init(&ctx, key, 256);
    uint8_t out[16];
    aes_decrypt_ecb(&ctx, ciphertext, out);
    ASSERT(memcmp(out, expected, 16) == 0);
}

/* NIST SP 800-38A CBC known-answer vectors (F.2.1/F.2.2 and F.2.5/F.2.6):
 * pin the CBC chaining itself, not just a round-trip. Decryption uses a
 * separate context so the decrypt schedule is exercised on its own. */

static const uint8_t sp800_iv[16] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
};

static const uint8_t sp800_pt[64] = {
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10
};

TEST(test_aes128_cbc_kat) {
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t expected_ct[64] = {
        0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d,
        0x50,0x86,0xcb,0x9b,0x50,0x72,0x19,0xee,0x95,0xdb,0x11,0x3a,0x91,0x76,0x78,0xb2,
        0x73,0xbe,0xd6,0xb8,0xe3,0xc1,0x74,0x3b,0x71,0x16,0xe6,0x9e,0x22,0x22,0x95,0x16,
        0x3f,0xf1,0xca,0xa1,0x68,0x1f,0xac,0x09,0x12,0x0e,0xca,0x30,0x75,0x86,0xe1,0xa7
    };
    uint8_t ct[64], pt[64];
    Aes enc;
    aes_init(&enc, key, 128);
    aes_encrypt_cbc(&enc, sp800_iv, sp800_pt, ct, 64);
    ASSERT(memcmp(ct, expected_ct, 64) == 0);
    Aes dec;
    aes_init(&dec, key, 128);
    aes_decrypt_cbc(&dec, sp800_iv, expected_ct, pt, 64);
    ASSERT(memcmp(pt, sp800_pt, 64) == 0);
}

TEST(test_aes256_cbc_kat) {
    const uint8_t key[32] = {
        0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,
        0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
        0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,
        0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
    };
    const uint8_t expected_ct[64] = {
        0xf5,0x8c,0x4c,0x04,0xd6,0xe5,0xf1,0xba,0x77,0x9e,0xab,0xfb,0x5f,0x7b,0xfb,0xd6,
        0x9c,0xfc,0x4e,0x96,0x7e,0xdb,0x80,0x8d,0x67,0x9f,0x77,0x7b,0xc6,0x70,0x2c,0x7d,
        0x39,0xf2,0x33,0x69,0xa9,0xd9,0xba,0xcf,0xa5,0x30,0xe2,0x63,0x04,0x23,0x14,0x61,
        0xb2,0xeb,0x05,0xe2,0xc3,0x9b,0xe9,0xfc,0xda,0x6c,0x19,0x07,0x8c,0x6a,0x9d,0x1b
    };
    uint8_t ct[64], pt[64];
    Aes enc;
    aes_init(&enc, key, 256);
    aes_encrypt_cbc(&enc, sp800_iv, sp800_pt, ct, 64);
    ASSERT(memcmp(ct, expected_ct, 64) == 0);
    Aes dec;
    aes_init(&dec, key, 256);
    aes_decrypt_cbc(&dec, sp800_iv, expected_ct, pt, 64);
    ASSERT(memcmp(pt, sp800_pt, 64) == 0);
}

/* Deterministic byte stream (LCG) so the long round-trip is reproducible. */
static void fill_pseudo_random(uint8_t *buf, size_t len, uint32_t seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < len; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 24);
    }
}

/* Multi-block CBC round-trip over an odd (non-power-of-two) block count:
 * 992 bytes = 62 blocks of pseudo-random data, encrypted with one context
 * and decrypted with a separately initialized one, at both key sizes. */
TEST(test_aes_cbc_long_roundtrip) {
    enum { LEN = 992 };
    uint8_t pt[LEN], ct[LEN], out[LEN];
    fill_pseudo_random(pt, LEN, 0xC0FFEEu);
    uint8_t key[32], iv[16];
    fill_pseudo_random(key, 32, 0xDEADBEEFu);
    fill_pseudo_random(iv, 16, 0x1234567u);

    for (int bits = 128; bits <= 256; bits += 128) {
        Aes enc, dec;
        aes_init(&enc, key, bits);
        aes_encrypt_cbc(&enc, iv, pt, ct, LEN);
        ASSERT(memcmp(ct, pt, LEN) != 0);
        aes_init(&dec, key, bits);
        aes_decrypt_cbc(&dec, iv, ct, out, LEN);
        ASSERT(memcmp(out, pt, LEN) == 0);
    }
}

TEST(test_aes128_cbc_roundtrip) {
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    const uint8_t iv[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    const uint8_t plaintext[32] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,
        0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x94
    };
    uint8_t encrypted[32], decrypted[32];
    Aes ctx;
    aes_init(&ctx, key, 128);
    aes_encrypt_cbc(&ctx, iv, plaintext, encrypted, 32);
    aes_decrypt_cbc(&ctx, iv, encrypted, decrypted, 32);
    ASSERT(memcmp(decrypted, plaintext, 32) == 0);
}

TEST(test_aes256_cbc_roundtrip) {
    const uint8_t key[32] = {
        0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,
        0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
        0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,
        0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
    };
    const uint8_t iv[16] = {0};
    const uint8_t plaintext[32] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
    };
    uint8_t encrypted[32], decrypted[32];
    Aes ctx;
    aes_init(&ctx, key, 256);
    aes_encrypt_cbc(&ctx, iv, plaintext, encrypted, 32);
    aes_decrypt_cbc(&ctx, iv, encrypted, decrypted, 32);
    ASSERT(memcmp(decrypted, plaintext, 32) == 0);
}

int main(void) {
    printf("Crypto tests:\n");
    printf("\n  MD5:\n");
    RUN(test_md5_empty);
    RUN(test_md5_a);
    RUN(test_md5_abc);
    RUN(test_md5_message_digest);
    RUN(test_md5_alphabet);
    RUN(test_md5_incremental);
    printf("\n  SHA-256:\n");
    RUN(test_sha256_empty);
    RUN(test_sha256_abc);
    RUN(test_sha256_long);
    RUN(test_sha256_incremental);
    printf("\n  SHA-512:\n");
    RUN(test_sha512_empty);
    RUN(test_sha512_abc);
    RUN(test_sha512_long);
    RUN(test_sha512_incremental);
    printf("\n  SHA-384:\n");
    RUN(test_sha384_empty);
    RUN(test_sha384_abc);
    RUN(test_sha384_long);
    RUN(test_sha384_incremental);

    printf("\n  RC4:\n");
    RUN(test_rc4_basic);
    RUN(test_rc4_wiki);
    RUN(test_rc4_roundtrip);

    printf("\n  AES:\n");
    RUN(test_aes128_ecb);
    RUN(test_aes256_ecb);
    RUN(test_aes128_ecb_decrypt_kat);
    RUN(test_aes256_ecb_decrypt_kat);
    RUN(test_aes128_cbc_kat);
    RUN(test_aes256_cbc_kat);
    RUN(test_aes_cbc_long_roundtrip);
    RUN(test_aes128_cbc_roundtrip);
    RUN(test_aes256_cbc_roundtrip);

    printf("\n%d tests, %d passed, %d failed, %d skipped\n",
           tests_run, tests_passed, tests_failed, tests_skipped);
    return tests_failed > 0 ? 1 : 0;
}
