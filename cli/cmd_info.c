#include "commands.h"
#include "../include/tspdf.h"
#include "../src/util/pdfdate.h"
#include <stdio.h>
#include <string.h>

// JSON string emission (same escaping rules as the web server's writer in
// server.c): control bytes as \u00XX, quote and backslash escaped, all
// other bytes verbatim (metadata getters return UTF-8).
static void json_print_string(const char *s) {
    putchar('"');
    for (const unsigned char *u = (const unsigned char *)s; *u; u++) {
        if (*u < 0x20u)
            printf("\\u%04x", (unsigned)*u);
        else if (*u == '"' || *u == '\\')
            printf("\\%c", (char)*u);
        else
            putchar((char)*u);
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
    } else {
        printf("\"encrypted\":false,\"encryption\":null,");
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

    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf info: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf info: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *input = positional[0];
    const char *password = find_flag(argc, argv, "--password");
    bool as_json = has_flag(argc, argv, "--json");

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
        for (size_t i = 0; i < page_count; i++) {
            TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
            if (page) {
                double unit = page->user_unit > 0.0 ? page->user_unit : 1.0;
                double w = (page->media_box[2] - page->media_box[0]) * unit;
                double h = (page->media_box[3] - page->media_box[1]) * unit;
                printf("  Page %zu:  %.1f x %.1f pt", i + 1, w, h);
                if (unit != 1.0)
                    printf(" (/UserUnit %.4g)", unit);
                if (page->rotate != 0)
                    printf(" (rotated %d°)", page->rotate);
                printf("\n");
            }
        }
    } else if (page_count > 5) {
        TspdfReaderPage *first = tspdf_reader_get_page(doc, 0);
        TspdfReaderPage *last = tspdf_reader_get_page(doc, page_count - 1);
        if (first) {
            double unit = first->user_unit > 0.0 ? first->user_unit : 1.0;
            double w = (first->media_box[2] - first->media_box[0]) * unit;
            double h = (first->media_box[3] - first->media_box[1]) * unit;
            printf("  Page 1:  %.1f x %.1f pt", w, h);
            if (unit != 1.0)
                printf(" (/UserUnit %.4g)", unit);
            printf("\n");
        }
        printf("  ...\n");
        if (last) {
            double unit = last->user_unit > 0.0 ? last->user_unit : 1.0;
            double w = (last->media_box[2] - last->media_box[0]) * unit;
            double h = (last->media_box[3] - last->media_box[1]) * unit;
            printf("  Page %zu: %.1f x %.1f pt", page_count, w, h);
            if (unit != 1.0)
                printf(" (/UserUnit %.4g)", unit);
            printf("\n");
        }
    }

    int enc_revision = 0;
    const char *enc_algorithm = NULL;
    if (tspdf_reader_encryption_info(doc, &enc_revision, &enc_algorithm))
        printf("Encrypted:  yes (%s, R%d)\n", enc_algorithm, enc_revision);
    else
        printf("Encrypted:  no\n");
    printf("Outlines:   %s\n", tspdf_reader_has_outlines(doc) ? "yes" : "no");
    printf("AcroForm:   %s\n", tspdf_reader_has_acroform(doc) ? "yes" : "no");

    tspdf_reader_destroy(doc);
    return 0;
}
