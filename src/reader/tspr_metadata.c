#define _POSIX_C_SOURCE 200809L
#include "tspr_internal.h"
#include "../util/pdftext.h"
#include <stdlib.h>
#include <string.h>

static TspdfObj *get_info_dict(const TspdfReader *doc) {
    if (!doc || !doc->xref.trailer) return NULL;
    TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
    if (!info_ref) return NULL;
    if (info_ref->type == TSPDF_OBJ_REF) {
        uint32_t num = info_ref->ref.num;
        if (num >= doc->xref.count) return NULL;
        if (doc->obj_cache[num]) return doc->obj_cache[num];
        // Not cached — resolve it
        TspdfReader *mut_doc = (TspdfReader *)doc;
        TspdfParser p;
        tspdf_parser_init(&p, mut_doc->data, mut_doc->data_len, &mut_doc->arena);
        return tspdf_xref_resolve(&mut_doc->xref, &p, num, mut_doc->obj_cache, mut_doc->crypt);
    }
    if (info_ref->type == TSPDF_OBJ_DICT) return info_ref;
    return NULL;
}

static const char *get_info_field(const TspdfReader *doc, const char *key) {
    TspdfObj *info = get_info_dict(doc);
    if (!info || info->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *val = tspdf_dict_get(info, key);
    if (!val || val->type != TSPDF_OBJ_STRING) return NULL;
    // UTF-16BE text strings (BOM FE FF, ISO 32000 §7.9.2.2) contain NUL bytes,
    // so decode them to UTF-8 for display; the arena keeps the copy alive for
    // the lifetime of the document. Other strings are returned as-is.
    TspdfReader *mut_doc = (TspdfReader *)doc;
    char *utf8 = tspdf_utf16be_to_utf8(val->string.data, val->string.len,
                                       &mut_doc->arena);
    if (utf8) return utf8;
    return (const char *)val->string.data;
}

const char *tspdf_reader_get_title(const TspdfReader *doc) { return get_info_field(doc, "Title"); }
const char *tspdf_reader_get_author(const TspdfReader *doc) { return get_info_field(doc, "Author"); }
const char *tspdf_reader_get_subject(const TspdfReader *doc) { return get_info_field(doc, "Subject"); }
const char *tspdf_reader_get_keywords(const TspdfReader *doc) { return get_info_field(doc, "Keywords"); }
const char *tspdf_reader_get_creator(const TspdfReader *doc) { return get_info_field(doc, "Creator"); }
const char *tspdf_reader_get_producer(const TspdfReader *doc) { return get_info_field(doc, "Producer"); }
const char *tspdf_reader_get_creation_date(const TspdfReader *doc) { return get_info_field(doc, "CreationDate"); }
const char *tspdf_reader_get_mod_date(const TspdfReader *doc) { return get_info_field(doc, "ModDate"); }

static void ensure_metadata(TspdfReader *doc) {
    if (!doc->metadata) {
        doc->metadata = calloc(1, sizeof(TspdfReaderMetadata));
    }
}

void tspdf_reader_set_title(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->title);
    doc->metadata->title = value ? strdup(value) : NULL;
    doc->metadata->title_set = true;
    doc->modified = true;
}
void tspdf_reader_set_author(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->author);
    doc->metadata->author = value ? strdup(value) : NULL;
    doc->metadata->author_set = true;
    doc->modified = true;
}
void tspdf_reader_set_subject(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->subject);
    doc->metadata->subject = value ? strdup(value) : NULL;
    doc->metadata->subject_set = true;
    doc->modified = true;
}
void tspdf_reader_set_keywords(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->keywords);
    doc->metadata->keywords = value ? strdup(value) : NULL;
    doc->metadata->keywords_set = true;
    doc->modified = true;
}
void tspdf_reader_set_creator(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->creator);
    doc->metadata->creator = value ? strdup(value) : NULL;
    doc->metadata->creator_set = true;
    doc->modified = true;
}
void tspdf_reader_set_producer(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->producer);
    doc->metadata->producer = value ? strdup(value) : NULL;
    doc->metadata->producer_set = true;
    doc->modified = true;
}

// --- Shared PDF text-encoding helpers (declared in ../util/pdftext.h) ---
//
// Implemented here rather than in a new src/util/*.c because the Makefile
// lists sources explicitly and tspr_metadata.c is already linked into every
// binary that needs these (CLI, reader tests, libtspdf).

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
