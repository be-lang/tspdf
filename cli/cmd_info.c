#include "commands.h"
#include "../include/tspdf.h"
#include "../src/util/pdfdate.h"
#include <stdio.h>
#include <string.h>

// Length of the valid UTF-8 sequence starting at u (1..4), or 0 if u does not
// begin a well-formed sequence — including overlong forms, out-of-range code
// points, and truncated sequences (a continuation byte missing or out of the
// 0x80..0xBF range). Metadata getters decode PDF text strings to UTF-8
// (src/util/pdftext.c), so this is a defensive second layer for strings from
// any other source.
static int utf8_seq_len(const unsigned char *u) {
    unsigned char c = u[0];
    if (c < 0x80u) return 1;
    if (c < 0xC2u) return 0;                 // 0x80..0xBF stray cont; 0xC0/0xC1 overlong
    if (c < 0xE0u) {                         // 2-byte
        if ((u[1] & 0xC0u) != 0x80u) return 0;
        return 2;
    }
    if (c < 0xF0u) {                         // 3-byte
        if ((u[1] & 0xC0u) != 0x80u || (u[2] & 0xC0u) != 0x80u) return 0;
        if (c == 0xE0u && u[1] < 0xA0u) return 0;                 // overlong
        if (c == 0xEDu && u[1] >= 0xA0u) return 0;                // surrogate
        return 3;
    }
    if (c < 0xF5u) {                         // 4-byte
        if ((u[1] & 0xC0u) != 0x80u || (u[2] & 0xC0u) != 0x80u ||
            (u[3] & 0xC0u) != 0x80u) return 0;
        if (c == 0xF0u && u[1] < 0x90u) return 0;                 // overlong
        if (c == 0xF4u && u[1] >= 0x90u) return 0;                // > U+10FFFF
        return 4;
    }
    return 0;                                // 0xF5..0xFF
}

// JSON string emission (same escaping rules as the web server's writer in
// server.c — keep them in sync): control bytes as \u00XX, quote and backslash
// escaped, valid UTF-8 passed through verbatim, and any byte that does not
// form well-formed UTF-8 replaced with U+FFFD so the output is always a
// decodable UTF-8 JSON string.
static void json_print_string(const char *s) {
    putchar('"');
    for (const unsigned char *u = (const unsigned char *)s; *u; ) {
        if (*u < 0x20u) {
            printf("\\u%04x", (unsigned)*u);
            u++;
        } else if (*u == '"' || *u == '\\') {
            printf("\\%c", (char)*u);
            u++;
        } else {
            int n = utf8_seq_len(u);
            if (n == 0) {
                fputs("\xEF\xBF\xBD", stdout);  // U+FFFD REPLACEMENT CHARACTER
                u++;
            } else {
                for (int i = 0; i < n; i++) putchar((char)u[i]);
                u += n;
            }
        }
    }
    putchar('"');
}

static void json_field_str(const char *key, const char *value, bool comma) {
    json_print_string(key);
    putchar(':');
    if (value)
        json_print_string(value);
    else
        printf("null");
    if (comma) putchar(',');
}

// Emit "<key>":<human>,"<key>_raw":<raw> — the parsed form when the value
// is a well-formed PDF date, the raw string as-is otherwise.
static void json_field_date(const char *key, const char *raw, bool comma) {
    char human[64];
    const char *value = raw;
    if (raw && tspdf_format_pdf_date(raw, human, sizeof(human)))
        value = human;
    json_field_str(key, value, true);
    char raw_key[32];
    snprintf(raw_key, sizeof(raw_key), "%s_raw", key);
    json_field_str(raw_key, raw, comma);
}

static void page_size(TspdfReaderPage *page, double *w, double *h) {
    double unit = page->user_unit > 0.0 ? page->user_unit : 1.0;
    *w = (page->media_box[2] - page->media_box[0]) * unit;
    *h = (page->media_box[3] - page->media_box[1]) * unit;
}

static void crop_size(TspdfReaderPage *page, double *w, double *h) {
    double unit = page->user_unit > 0.0 ? page->user_unit : 1.0;
    *w = (page->crop_box[2] - page->crop_box[0]) * unit;
    *h = (page->crop_box[3] - page->crop_box[1]) * unit;
}

// User-access permission bits, ISO 32000-1 Table 22 — same names (and bit
// values) as `tspdf encrypt --permissions` in cmd_encrypt.c.
static const struct {
    const char *name;
    uint32_t bit;
} PERM_BITS[] = {
    {"print",    1u << 2},   // bit 3: print the document
    {"print-hq", 1u << 11},  // bit 12: high-resolution printing
    {"copy",     1u << 4},   // bit 5: copy text and graphics
    {"extract",  1u << 9},   // bit 10: extract for accessibility
    {"modify",   1u << 3},   // bit 4: modify contents
    {"annotate", 1u << 5},   // bit 6: add/modify annotations
    {"forms",    1u << 8},   // bit 9: fill in form fields
    {"assemble", 1u << 10},  // bit 11: insert/rotate/delete pages
};
#define PERM_BITS_COUNT (sizeof(PERM_BITS) / sizeof(PERM_BITS[0]))

// One "  Page N:  W x H pt" line of the text output, with CropBox, /UserUnit
// and rotation notes when relevant.
static void print_page_line(TspdfReaderPage *page, size_t num) {
    if (!page) return;
    double w, h;
    page_size(page, &w, &h);
    printf("  Page %zu:  %.1f x %.1f pt", num, w, h);
    if (page->has_crop_box) {
        double cw, ch;
        crop_size(page, &cw, &ch);
        printf(" (CropBox %.1f x %.1f pt)", cw, ch);
    }
    double unit = page->user_unit > 0.0 ? page->user_unit : 1.0;
    if (unit != 1.0)
        printf(" (/UserUnit %.4g)", unit);
    if (page->rotate != 0)
        printf(" (rotated %d°)", page->rotate);
    printf("\n");
}

// {"error":"<message>"} on stdout — the failure shape of `info --json`.
static void json_error(const char *message) {
    printf("{");
    json_field_str("error", message, false);
    printf("}\n");
}

static int print_json(TspdfReader *doc) {
    size_t page_count = tspdf_reader_page_count(doc);

    printf("{");
    json_field_str("version", tspdf_reader_pdf_version(doc), true);
    printf("\"pages\":%zu,", page_count);

    int enc_revision = 0;
    const char *enc_algorithm = NULL;
    if (tspdf_reader_encryption_info(doc, &enc_revision, &enc_algorithm)) {
        char enc[64];
        snprintf(enc, sizeof(enc), "%s (R%d)", enc_algorithm, enc_revision);
        printf("\"encrypted\":true,");
        json_field_str("encryption", enc, true);
        uint32_t perms = 0;
        tspdf_reader_encryption_permissions(doc, &perms);
        printf("\"permissions\":[");
        bool first = true;
        for (size_t i = 0; i < PERM_BITS_COUNT; i++) {
            if (perms & PERM_BITS[i].bit) {
                printf("%s\"%s\"", first ? "" : ",", PERM_BITS[i].name);
                first = false;
            }
        }
        // The raw /P value as the signed 32-bit integer written to the file.
        printf("],\"permissions_raw\":%d,", (int32_t)perms);
    } else {
        printf("\"encrypted\":false,\"encryption\":null,"
               "\"permissions\":null,\"permissions_raw\":null,");
    }
    printf("\"outlines\":%s,", tspdf_reader_has_outlines(doc) ? "true" : "false");
    printf("\"acroform\":%s,", tspdf_reader_has_acroform(doc) ? "true" : "false");

    // A single [w,h] when every page has the same size, else one per page.
    bool uniform = page_count > 0;
    double w0 = 0.0, h0 = 0.0;
    for (size_t i = 0; i < page_count && uniform; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) { uniform = false; break; }
        double w, h;
        page_size(page, &w, &h);
        if (i == 0) { w0 = w; h0 = h; }
        else if (w != w0 || h != h0) uniform = false;
    }
    printf("\"page_sizes\":");
    if (uniform) {
        printf("[%.2f,%.2f]", w0, h0);
    } else {
        printf("[");
        for (size_t i = 0; i < page_count; i++) {
            TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
            double w = 0.0, h = 0.0;
            if (page) page_size(page, &w, &h);
            printf("%s[%.2f,%.2f]", i > 0 ? "," : "", w, h);
        }
        printf("]");
    }
    printf(",");

    // CropBox sizes: null when no page has one; a single [w,h] when every
    // page has the same crop; else one entry per page ([w,h] or null).
    bool any_crop = false, crop_uniform = page_count > 0;
    double cw0 = 0.0, ch0 = 0.0;
    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page || !page->has_crop_box) { crop_uniform = false; continue; }
        any_crop = true;
        double w, h;
        crop_size(page, &w, &h);
        if (i == 0) { cw0 = w; ch0 = h; }
        else if (w != cw0 || h != ch0) crop_uniform = false;
    }
    printf("\"crop_sizes\":");
    if (!any_crop) {
        printf("null");
    } else if (crop_uniform) {
        printf("[%.2f,%.2f]", cw0, ch0);
    } else {
        printf("[");
        for (size_t i = 0; i < page_count; i++) {
            TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
            if (i > 0) printf(",");
            if (page && page->has_crop_box) {
                double w, h;
                crop_size(page, &w, &h);
                printf("[%.2f,%.2f]", w, h);
            } else {
                printf("null");
            }
        }
        printf("]");
    }
    printf(",");

    json_field_str("title", tspdf_reader_get_title(doc), true);
    json_field_str("author", tspdf_reader_get_author(doc), true);
    json_field_str("subject", tspdf_reader_get_subject(doc), true);
    json_field_str("keywords", tspdf_reader_get_keywords(doc), true);
    json_field_str("creator", tspdf_reader_get_creator(doc), true);
    json_field_str("producer", tspdf_reader_get_producer(doc), true);
    json_field_date("created", tspdf_reader_get_creation_date(doc), true);
    json_field_date("modified", tspdf_reader_get_mod_date(doc), false);
    printf("}\n");
    return 0;
}

int cmd_info(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        printf("Usage: tspdf info <input.pdf> [--password <pass>] [--json]\n");
        printf("\nPrint information about a PDF file.\n");
        printf("  --json    Machine-readable JSON output (includes metadata)\n");
        return argc == 0 ? 1 : 0;
    }

    bool as_json = has_flag(argc, argv, "--json");

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf info: missing input file\n");
        if (as_json) json_error("missing input file");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf info: unexpected extra argument '%s'\n", positional[1]);
        if (as_json) json_error("unexpected extra argument");
        return 1;
    }
    const char *input = positional[0];
    const char *password = find_flag(argc, argv, "--password");

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);

    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            if (as_json) {
                printf("{\"encrypted\":true,\"error\":\"password required\"}\n");
                return 0;
            }
            printf("File:      %s\n", input);
            printf("Encrypted: yes\n");
            printf("\nThis PDF is encrypted. Use --password <pass> to open it.\n");
            return 0;
        }
        fprintf(stderr, "tspdf info: failed to open '%s': %s\n", input, tspdf_error_string(err));
        if (as_json) {
            // Machine consumers read stdout: always give them a JSON object.
            printf("{");
            if (err == TSPDF_ERR_BAD_PASSWORD)
                printf("\"encrypted\":true,");
            json_field_str("error", tspdf_error_string(err), false);
            printf("}\n");
        }
        return 1;
    }

    if (as_json) {
        int rc = print_json(doc);
        tspdf_reader_destroy(doc);
        return rc;
    }

    size_t page_count = tspdf_reader_page_count(doc);

    printf("File:       %s\n", input);
    const char *pdf_version = tspdf_reader_pdf_version(doc);
    if (pdf_version && pdf_version[0])
        printf("Version:    PDF %s\n", pdf_version);
    printf("Pages:      %zu\n", page_count);

    // Show page sizes
    if (page_count > 0 && page_count <= 5) {
        for (size_t i = 0; i < page_count; i++)
            print_page_line(tspdf_reader_get_page(doc, i), i + 1);
    } else if (page_count > 5) {
        print_page_line(tspdf_reader_get_page(doc, 0), 1);
        printf("  ...\n");
        print_page_line(tspdf_reader_get_page(doc, page_count - 1), page_count);
    }

    int enc_revision = 0;
    const char *enc_algorithm = NULL;
    if (tspdf_reader_encryption_info(doc, &enc_revision, &enc_algorithm)) {
        printf("Encrypted:  yes (%s, R%d)\n", enc_algorithm, enc_revision);
        uint32_t perms = 0;
        tspdf_reader_encryption_permissions(doc, &perms);
        printf("Permissions: ");
        bool first = true;
        for (size_t i = 0; i < PERM_BITS_COUNT; i++) {
            if (perms & PERM_BITS[i].bit) {
                printf("%s%s", first ? "" : ", ", PERM_BITS[i].name);
                first = false;
            }
        }
        if (first) printf("(none)");
        printf(" (P=%d)\n", (int32_t)perms);
    } else {
        printf("Encrypted:  no\n");
    }
    printf("Outlines:   %s\n", tspdf_reader_has_outlines(doc) ? "yes" : "no");
    printf("AcroForm:   %s\n", tspdf_reader_has_acroform(doc) ? "yes" : "no");

    tspdf_reader_destroy(doc);
    return 0;
}
