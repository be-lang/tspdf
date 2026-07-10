#ifndef QR_ENCODE_H
#define QR_ENCODE_H

#include <stdint.h>
#include <stddef.h>

/* QR code result */
typedef struct {
    uint8_t *modules; /* size x size matrix (1 = black, 0 = white), row-major */
    int size;         /* dimension (21 for version 1, 25 for version 2, etc.) */
} QrCode;

/* Error correction level (ISO/IEC 18004): L recovers ~7% of codewords,
 * M ~15%, Q ~25%, H ~30%. Higher levels shrink the data capacity. */
typedef enum {
    QR_EC_L = 0,
    QR_EC_M = 1,
    QR_EC_Q = 2,
    QR_EC_H = 3,
} QrEcLevel;

/*
 * Encode text into a QR code using byte mode, ECC level M.
 * Returns NULL on failure (text too long or allocation error).
 * Caller must free the result with qr_free().
 */
QrCode *qr_encode(const char *text);

/* Same as qr_encode with an explicit error correction level. */
QrCode *qr_encode_level(const char *text, QrEcLevel level);

/* Free a QR code returned by qr_encode(). */
void qr_free(QrCode *qr);

/*
 * Introspection hooks for the unit tests. Not part of the public library
 * surface; they expose internal encoder state so the tests can check the
 * ECC block table, Reed-Solomon output, and version-information bits
 * against ISO/IEC 18004 reference data.
 */

/*
 * ECC block structure for `version` at `level`. Returns 0 on success,
 * -1 if the version or level is out of range. Any output pointer may be
 * NULL.
 */
int qr_ecc_block_info(int version, QrEcLevel level,
                      int *total_cw, int *ecc_per_block,
                      int *blocks_g1, int *data_g1,
                      int *blocks_g2, int *data_g2);

/* Reed-Solomon EC codewords over GF(256) with polynomial 0x11D. */
void qr_rs_ecc(const uint8_t *data, int data_len, uint8_t *ecc, int num_ecc);

/* 18-bit BCH(18,6) version information value; 0 for versions < 7. */
uint32_t qr_version_info_bits(int version);

/* Highest supported version (capacity ceiling for qr_encode). */
int qr_max_version(void);

#endif /* QR_ENCODE_H */
