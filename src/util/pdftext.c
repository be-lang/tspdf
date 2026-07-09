// Shared PDF text-encoding helpers (declared in pdftext.h): UTF-8 decode/
// encode, cp1252 mapping, and Info-dictionary text strings. Linked into
// every binary (LIB_SOURCES), so both the writer and the reader can use them.

#include "pdftext.h"

size_t tspdf_utf8_decode(const char *s, uint32_t *out_cp) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp;
    size_t len;

    if (p[0] < 0x80) {
        cp = p[0];
        len = 1;
    } else if ((p[0] & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) return 0;
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        if (cp < 0x80) return 0;  // overlong
        len = 2;
    } else if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        cp = ((uint32_t)(p[0] & 0x0F) << 12) |
             ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        if (cp < 0x800) return 0;                    // overlong
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  // surrogate
        len = 3;
    } else if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) return 0;
        cp = ((uint32_t)(p[0] & 0x07) << 18) |
             ((uint32_t)(p[1] & 0x3F) << 12) |
             ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return 0;  // overlong / out of range
        len = 4;
    } else {
        return 0;  // stray continuation byte or invalid lead byte
    }

    if (out_cp) *out_cp = cp;
    return len;
}

size_t tspdf_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        out[0] = '?';  // lone surrogate: not representable
        return 1;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

int tspdf_cp1252_from_codepoint(uint32_t cp) {
    if (cp < 0x80) return (int)cp;                  // ASCII
    if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;   // Latin-1 range is identity
    // The 0x80-0x9F window maps to specials (five slots are unassigned).
    switch (cp) {
        case 0x20AC: return 0x80;  // €
        case 0x201A: return 0x82;  // ‚
        case 0x0192: return 0x83;  // ƒ
        case 0x201E: return 0x84;  // „
        case 0x2026: return 0x85;  // …
        case 0x2020: return 0x86;  // †
        case 0x2021: return 0x87;  // ‡
        case 0x02C6: return 0x88;  // ˆ
        case 0x2030: return 0x89;  // ‰
        case 0x0160: return 0x8A;  // Š
        case 0x2039: return 0x8B;  // ‹
        case 0x0152: return 0x8C;  // Œ
        case 0x017D: return 0x8E;  // Ž
        case 0x2018: return 0x91;  // '
        case 0x2019: return 0x92;  // '
        case 0x201C: return 0x93;  // "
        case 0x201D: return 0x94;  // "
        case 0x2022: return 0x95;  // •
        case 0x2013: return 0x96;  // – (en-dash)
        case 0x2014: return 0x97;  // — (em-dash)
        case 0x02DC: return 0x98;  // ˜
        case 0x2122: return 0x99;  // ™
        case 0x0161: return 0x9A;  // š
        case 0x203A: return 0x9B;  // ›
        case 0x0153: return 0x9C;  // œ
        case 0x017E: return 0x9E;  // ž
        case 0x0178: return 0x9F;  // Ÿ
        default: return -1;
    }
}

int tspdf_utf8_to_cp1252(const char *utf8, char *out, uint32_t *bad_cp) {
    size_t o = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        size_t n = tspdf_utf8_decode(p, &cp);
        if (n == 0) {
            if (bad_cp) *bad_cp = (uint8_t)*p;
            return TSPDF_PDFTEXT_BAD_UTF8;
        }
        int b = tspdf_cp1252_from_codepoint(cp);
        if (b < 0) {
            if (bad_cp) *bad_cp = cp;
            return TSPDF_PDFTEXT_UNMAPPED;
        }
        out[o++] = (char)b;
        p += n;
    }
    out[o] = '\0';
    return TSPDF_PDFTEXT_OK;
}

size_t tspdf_utf8_to_cp1252_lossy(const char *utf8, char *out, uint32_t *first_bad_cp) {
    size_t o = 0;
    size_t subs = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        size_t n = tspdf_utf8_decode(p, &cp);
        if (n == 0) {
            // Malformed byte: substitute and resync on the next byte.
            if (subs++ == 0 && first_bad_cp) *first_bad_cp = (uint8_t)*p;
            out[o++] = '?';
            p++;
            continue;
        }
        int b = tspdf_cp1252_from_codepoint(cp);
        if (b < 0) {
            if (subs++ == 0 && first_bad_cp) *first_bad_cp = cp;
            b = '?';
        }
        out[o++] = (char)b;
        p += n;
    }
    out[o] = '\0';
    return subs;
}

bool tspdf_str_is_ascii(const char *s) {
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        if (*p >= 0x80) return false;
    }
    return true;
}

// Escaped literal string, mirroring the serializer's escaping rules.
static void pdftext_write_literal(TspdfBuffer *buf, const char *value) {
    tspdf_buffer_append_byte(buf, '(');
    for (const uint8_t *p = (const uint8_t *)value; *p; p++) {
        uint8_t c = *p;
        switch (c) {
            case '(':  tspdf_buffer_append_str(buf, "\\("); break;
            case ')':  tspdf_buffer_append_str(buf, "\\)"); break;
            case '\\': tspdf_buffer_append_str(buf, "\\\\"); break;
            case '\n': tspdf_buffer_append_str(buf, "\\n"); break;
            case '\r': tspdf_buffer_append_str(buf, "\\r"); break;
            case '\t': tspdf_buffer_append_str(buf, "\\t"); break;
            case '\b': tspdf_buffer_append_str(buf, "\\b"); break;
            case '\f': tspdf_buffer_append_str(buf, "\\f"); break;
            default:
                if (c < 32 || c > 126) {
                    tspdf_buffer_printf(buf, "\\%03o", c);
                } else {
                    tspdf_buffer_append_byte(buf, c);
                }
                break;
        }
    }
    tspdf_buffer_append_byte(buf, ')');
}

void tspdf_pdftext_write_info_string(TspdfBuffer *buf, const char *value) {
    if (!tspdf_str_is_ascii(value)) {
        // Validate the whole string first so a malformed tail cannot leave a
        // half-written hex string behind.
        bool valid = true;
        for (const char *p = value; *p;) {
            size_t n = tspdf_utf8_decode(p, NULL);
            if (n == 0) { valid = false; break; }
            p += n;
        }
        if (valid) {
            // BOM-prefixed UTF-16BE hex string (no escaping needed).
            tspdf_buffer_append_str(buf, "<FEFF");
            for (const char *p = value; *p;) {
                uint32_t cp;
                p += tspdf_utf8_decode(p, &cp);
                if (cp >= 0x10000) {
                    uint32_t v = cp - 0x10000;
                    tspdf_buffer_printf(buf, "%04X%04X",
                                        0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else {
                    tspdf_buffer_printf(buf, "%04X", cp);
                }
            }
            tspdf_buffer_append_byte(buf, '>');
            return;
        }
    }
    // Pure ASCII, or a byte string that is not valid UTF-8: keep the literal
    // form so nothing is reinterpreted.
    pdftext_write_literal(buf, value);
}

char *tspdf_utf16be_to_utf8(const uint8_t *data, size_t len, TspdfArena *arena) {
    if (!data || len < 2 || data[0] != 0xFE || data[1] != 0xFF) return NULL;
    // Worst case: each 2-byte unit becomes 3 UTF-8 bytes (a 4-byte surrogate
    // pair becomes 4 bytes, which is smaller per input byte).
    size_t cap = (len / 2) * 3 + 1;
    char *out = (char *)tspdf_arena_alloc(arena, cap);
    if (!out) return NULL;

    size_t o = 0;
    size_t i = 2;
    while (i + 1 < len) {
        uint32_t cp = ((uint32_t)data[i] << 8) | data[i + 1];
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
            uint32_t lo = ((uint32_t)data[i] << 8) | data[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        o += tspdf_utf8_encode(cp, out + o);  // lone surrogates become '?'
    }
    out[o] = '\0';  // a trailing odd byte is ignored
    return out;
}
