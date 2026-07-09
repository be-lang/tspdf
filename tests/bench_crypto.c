/* AES-CBC throughput micro-benchmark for the in-tree implementation.
 *
 * Times encrypt and decrypt over a ~23 MB buffer (a realistic large encrypted
 * PDF payload) at both key sizes and prints MB/s (best of 3 runs). Build and
 * run with `make bench-crypto`. Not part of test-all: it measures, it does
 * not gate — though it does verify the round-trip before trusting a timing.
 */
#define _POSIX_C_SOURCE 199309L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/crypto/aes.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define REPS 3

int main(void) {
    const size_t LEN = 23u * 1024 * 1024;
    uint8_t *pt = malloc(LEN), *ct = malloc(LEN), *out = malloc(LEN);
    if (!pt || !ct || !out) {
        fprintf(stderr, "bench_crypto: allocation failed\n");
        return 1;
    }

    /* Deterministic input so runs are comparable. */
    srand(42);
    for (size_t i = 0; i < LEN; i++) pt[i] = (uint8_t)rand();
    uint8_t key[32], iv[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 3);
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(200 - i);

    double mb = (double)LEN / (1024.0 * 1024.0);
    printf("AES-CBC throughput, %.0f MB buffer, best of %d runs\n\n", mb, REPS);
    printf("%-8s %-4s %10s %12s\n", "cipher", "op", "best s", "MB/s");

    for (int bits = 128; bits <= 256; bits += 128) {
        Aes aes;
        aes_init(&aes, key, bits);

        double best = 1e30;
        for (int r = 0; r < REPS; r++) {
            double s = now_s();
            aes_encrypt_cbc(&aes, iv, pt, ct, LEN);
            double t = now_s() - s;
            if (t < best) best = t;
        }
        printf("AES-%-4d %-4s %10.3f %12.2f\n", bits, "enc", best, mb / best);

        best = 1e30;
        for (int r = 0; r < REPS; r++) {
            double s = now_s();
            aes_decrypt_cbc(&aes, iv, ct, out, LEN);
            double t = now_s() - s;
            if (t < best) best = t;
        }
        if (memcmp(out, pt, LEN) != 0) {
            fprintf(stderr, "bench_crypto: AES-%d round-trip MISMATCH\n", bits);
            return 1;
        }
        printf("AES-%-4d %-4s %10.3f %12.2f\n", bits, "dec", best, mb / best);
    }

    free(pt);
    free(ct);
    free(out);
    return 0;
}
