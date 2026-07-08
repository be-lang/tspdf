#ifndef QR_ENCODE_H
#define QR_ENCODE_H

#include <stdint.h>
#include <stddef.h>

/* QR code result */
typedef struct {
    uint8_t *modules; /* size x size matrix (1 = black, 0 = white), row-major */
    int size;         /* dimension (21 for version 1, 25 for version 2, etc.) */
} QrCode;

/*
 * Encode text into a QR code using byte mode, ECC level M.
 * Returns NULL on failure (text too long or allocation error).
 * Caller must free the result with qr_free().
 */
QrCode *qr_encode(const char *text);

/* Free a QR code returned by qr_encode(). */
void qr_free(QrCode *qr);

#endif /* QR_ENCODE_H */
