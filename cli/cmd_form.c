// tspdf form — list, fill, and flatten AcroForm form fields.
//
//   tspdf form list <in.pdf> [--password p]
//   tspdf form fill <in.pdf> --data <fields.json|-> [--set name=value ...]
//                   -o <out.pdf> [--password p] [--force]
//   tspdf form flatten <in.pdf> -o <out.pdf> [--password p]
//
// list prints one JSON array to stdout; fill takes a flat JSON object of
// field name -> string value (strict parser below) and/or repeated --set
// pairs for shell use.

#define _POSIX_C_SOURCE 200809L  // strdup, strndup

#include "commands.h"
#include "../include/tspdf.h"
#include "../src/util/pdftext.h"
#include "password_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FORM_MAX_SETS 256

static void form_usage(void) {
    printf("Usage: tspdf form <list|fill|flatten> <input.pdf> [options]\n");
    printf("\n");
    printf("Work with AcroForm form fields.\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  list <input.pdf>                    Print all fields as JSON\n");
    printf("  fill <input.pdf> -o <output.pdf>    Set field values\n");
    printf("      --data <file.json|->            JSON object {\"field\": \"value\", ...}\n");
    printf("                                      ('-' reads it from stdin)\n");
    printf("      --set name=value                Set one field (repeatable)\n");
    printf("      --force                         Also fill readonly fields\n");
    printf("  flatten <input.pdf> -o <output.pdf> Stamp values into the page\n");
    printf("                                      content and remove the form\n");
    printf("\n");
    printf("Options:\n");
    printf("  --password <pass>       Password for encrypted PDFs (optional).\n");
    printf("  --password-file <file>  Read it from the first line of a file\n");
    printf("                          ('-' reads stdin) instead.\n");
    printf("                          Encrypted inputs stay encrypted on output,\n");
    printf("                          with the same passwords.\n");
    printf("\n");
    printf("Checkboxes and radio groups take an \"on\" state name (see the\n");
    printf("options array in `form list`), \"Off\", or true/false. Choice\n");
    printf("fields only accept their listed options, unless the combo is\n");
    printf("editable. Text appearances are single-line (no comb/multiline\n");
    printf("layout).\n");
    printf("\n");
    printf("Values with characters outside WinAnsi (CJK, Greek, ...) are\n");
    printf("rendered with a fallback TrueType font: set\n");
    printf("TSPDF_FALLBACK_FONT=/path/to/font.ttf, or let tspdf scan the\n");
    printf("system font directories (TSPDF_FONT_DIRS overrides the scan\n");
    printf("roots). Without a usable font those characters display as '?'\n");
    printf("(the stored value keeps them).\n");
}

// --- JSON string output ---

static void form_json_print_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 0x20) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

// --- strict flat JSON parser: { "name": "value", ... } ---
//
// Only what `--data` needs: one object, string keys, string values, \uXXXX
// escapes (with surrogate pairs). Anything else is a hard error — better to
// reject a typo'd file than to silently fill nothing.

typedef struct {
    char **keys;
    char **vals;
    size_t count;
    size_t cap;
} FormPairs;

static void form_pairs_free(FormPairs *d) {
    for (size_t i = 0; i < d->count; i++) {
        free(d->keys[i]);
        free(d->vals[i]);
    }
    free(d->keys);
    free(d->vals);
    memset(d, 0, sizeof(*d));
}

static bool form_pairs_add(FormPairs *d, char *key, char *val) {
    if (d->count >= d->cap) {
        size_t cap = d->cap == 0 ? 16 : d->cap * 2;
        char **nk = realloc(d->keys, cap * sizeof(char *));
        char **nv = realloc(d->vals, cap * sizeof(char *));
        if (nk) d->keys = nk;
        if (nv) d->vals = nv;
        if (!nk || !nv) return false;
        d->cap = cap;
    }
    d->keys[d->count] = key;
    d->vals[d->count] = val;
    d->count++;
    return true;
}

static const char *form_json_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int form_json_hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static bool form_json_out(char **buf, size_t *len, size_t *cap, const char *b,
                          size_t n) {
    if (*len + n + 1 > *cap) {
        size_t grown = *cap == 0 ? 32 : *cap;
        while (grown < *len + n + 1) grown *= 2;
        char *nb = realloc(*buf, grown);
        if (!nb) return false;
        *buf = nb;
        *cap = grown;
    }
    memcpy(*buf + *len, b, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static size_t form_utf8_encode(unsigned cp, char out[4]) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// Parse a JSON string at *pp (must point at '"'). Returns a malloc'd UTF-8
// string and advances *pp past the closing quote, or NULL on syntax error.
static char *form_json_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    if (!form_json_out(&buf, &len, &cap, "", 0)) return NULL;
    while (*p && *p != '"') {
        if ((unsigned char)*p < 0x20) {  // raw control chars are not JSON
            free(buf);
            return NULL;
        }
        if (*p == '\\') {
            char e = p[1];
            char c;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (form_json_hex4(p + 2, &cp) != 0) {
                        free(buf);
                        return NULL;
                    }
                    p += 6;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' &&
                        p[1] == 'u') {
                        unsigned lo;
                        if (form_json_hex4(p + 2, &lo) == 0 && lo >= 0xDC00 &&
                            lo <= 0xDFFF) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) |
                                             (lo - 0xDC00u));
                            p += 6;
                        }
                    }
                    // Unpaired surrogates and NUL would corrupt the C string.
                    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
                    char enc[4];
                    size_t n = form_utf8_encode(cp, enc);
                    if (!form_json_out(&buf, &len, &cap, enc, n)) {
                        free(buf);
                        return NULL;
                    }
                    continue;
                }
                default:
                    free(buf);
                    return NULL;
            }
            if (!form_json_out(&buf, &len, &cap, &c, 1)) {
                free(buf);
                return NULL;
            }
            p += 2;
        } else {
            if (!form_json_out(&buf, &len, &cap, p, 1)) {
                free(buf);
                return NULL;
            }
            p++;
        }
    }
    if (*p != '"') {
        free(buf);
        return NULL;
    }
    *pp = p + 1;
    return buf;
}

// Parse the whole --data document into pairs. Returns false with a message
// on stderr for anything that is not a flat string->string object.
static bool form_parse_data(const char *json, FormPairs *pairs) {
    const char *p = form_json_ws(json);
    if (*p != '{') {
        fprintf(stderr, "tspdf form: --data must be a JSON object\n");
        return false;
    }
    p = form_json_ws(p + 1);
    if (*p == '}') {
        p = form_json_ws(p + 1);
        if (*p != '\0') {
            fprintf(stderr, "tspdf form: trailing data after JSON object\n");
            return false;
        }
        return true;
    }
    for (;;) {
        char *key = form_json_string(&p);
        if (!key) {
            fprintf(stderr, "tspdf form: invalid JSON key (strings only)\n");
            return false;
        }
        p = form_json_ws(p);
        if (*p != ':') {
            free(key);
            fprintf(stderr, "tspdf form: expected ':' after JSON key\n");
            return false;
        }
        p = form_json_ws(p + 1);
        char *val = form_json_string(&p);
        if (!val) {
            free(key);
            fprintf(stderr, "tspdf form: field values must be JSON strings\n");
            return false;
        }
        if (!form_pairs_add(pairs, key, val)) {
            free(key);
            free(val);
            fprintf(stderr, "tspdf form: out of memory\n");
            return false;
        }
        p = form_json_ws(p);
        if (*p == ',') {
            p = form_json_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            p = form_json_ws(p + 1);
            if (*p != '\0') {
                fprintf(stderr, "tspdf form: trailing data after JSON object\n");
                return false;
            }
            return true;
        }
        fprintf(stderr, "tspdf form: expected ',' or '}' in JSON object\n");
        return false;
    }
}

// Read a whole file ('-' = stdin) into a malloc'd NUL-terminated buffer.
static char *form_read_all(const char *path) {
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        if (f != stdin) fclose(f);
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                if (f != stdin) fclose(f);
                return NULL;
            }
            buf = nb;
        }
    }
    if (f != stdin) fclose(f);
    buf[len] = '\0';
    if (memchr(buf, '\0', len)) {  // NUL bytes would hide trailing data
        free(buf);
        return NULL;
    }
    return buf;
}

// --- shared open helper ---

static TspdfReader *form_open(const char *input, const char *password) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf form: '%s' is encrypted; use --password or --password-file\n",
                    input);
        } else {
            fprintf(stderr, "tspdf form: failed to open '%s': %s\n", input,
                    tspdf_error_string(err));
        }
    }
    return doc;
}

static const char *form_type_string(TspdfFieldType type) {
    switch (type) {
        case TSPDF_FIELD_TEXT: return "text";
        case TSPDF_FIELD_CHECKBOX: return "checkbox";
        case TSPDF_FIELD_RADIO: return "radio";
        case TSPDF_FIELD_PUSHBUTTON: return "pushbutton";
        case TSPDF_FIELD_CHOICE: return "choice";
        case TSPDF_FIELD_SIGNATURE: return "signature";
        default: return "unknown";
    }
}

// --- list ---

static int form_list(const char *input, const char *password) {
    TspdfReader *doc = form_open(input, password);
    if (!doc) return 1;

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(doc, &fields, &count);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf form: cannot read form fields: %s\n",
                tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    printf("[");
    for (size_t i = 0; i < count; i++) {
        const TspdfFormFieldInfo *f = &fields[i];
        printf(i == 0 ? "\n  {" : ",\n  {");
        printf("\"name\": ");
        form_json_print_string(f->name);
        printf(", \"type\": \"%s\", \"value\": ", form_type_string(f->type));
        if (f->value) form_json_print_string(f->value);
        else printf("null");
        if (f->page_index == (size_t)-1) {
            printf(", \"page\": null");
        } else {
            printf(", \"page\": %zu", f->page_index + 1);
        }
        printf(", \"required\": %s", f->required ? "true" : "false");
        printf(", \"readonly\": %s", f->readonly ? "true" : "false");
        if (f->option_count > 0) {
            printf(", \"options\": [");
            for (size_t j = 0; j < f->option_count; j++) {
                if (j > 0) printf(", ");
                form_json_print_string(f->options[j] ? f->options[j] : "");
            }
            printf("]");
        }
        printf("}");
    }
    printf(count > 0 ? "\n]\n" : "]\n");

    tspdf_reader_destroy(doc);
    return 0;
}

// --- fill ---

static const TspdfFormFieldInfo *form_find_field(
        const TspdfFormFieldInfo *fields, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (fields[i].name && strcmp(fields[i].name, name) == 0) {
            return &fields[i];
        }
    }
    return NULL;
}

// Generated text appearances (fill) and flatten stamping render values with
// a WinAnsi (cp1252) base font; characters outside it are drawn with an
// embedded fallback TrueType font when one is discoverable on this machine
// (TSPDF_FALLBACK_FONT override, else a system font scan). Only when neither
// works do they display as '?' — the stored /V keeps the full value, so that
// is a visual-only loss, but a silent one, hence the warning.
static void form_warn_lossy_value(TspdfReader *doc, const char *name,
                                  const char *value) {
    if (tspdf_str_is_ascii(value)) return;
    size_t len = strlen(value);
    char *scratch = malloc(len + 1);  // cp1252 is never longer than UTF-8
    if (!scratch) return;
    size_t subs = tspdf_utf8_to_cp1252_lossy(value, scratch, NULL);
    free(scratch);
    if (subs == 0) return;
    if (tspdf_reader_form_value_renderable(doc, value)) return;
    fprintf(stderr,
            "tspdf form: warning: value for field '%s' contains "
            "characters not representable in the appearance font and no "
            "fallback TrueType font was found; they will display as '?' "
            "(the stored value is intact). Set TSPDF_FALLBACK_FONT=</path/"
            "to/font.ttf> to embed one.\n",
            name);
}

// Checkbox/radio convenience: true/false (and friends) map to the first
// "on" state / "Off"; anything else passes through as a state name.
static const char *form_map_button_value(const TspdfFormFieldInfo *f,
                                         const char *value) {
    if (f->type != TSPDF_FIELD_CHECKBOX && f->type != TSPDF_FIELD_RADIO) {
        return value;
    }
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
        return f->option_count > 0 ? f->options[0] : "Yes";
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        return "Off";
    }
    return value;
}

static int form_fill(int argc, char **argv, const char *input,
                     const char *output, const char *password) {
    if (!output) {
        fprintf(stderr, "tspdf form: missing output file (-o)\n");
        return 1;
    }
    bool force = has_flag(argc, argv, "--force");
    const char *data_arg = find_flag(argc, argv, "--data");
    const char *sets[FORM_MAX_SETS];
    int nsets = find_flags(argc, argv, "--set", sets, FORM_MAX_SETS);
    if (!data_arg && nsets == 0) {
        fprintf(stderr, "tspdf form: nothing to fill (use --data or --set)\n");
        return 1;
    }

    FormPairs pairs = {0};
    if (data_arg) {
        char *json = form_read_all(data_arg);
        if (!json) {
            fprintf(stderr, "tspdf form: cannot read --data '%s'\n", data_arg);
            return 1;
        }
        bool ok = form_parse_data(json, &pairs);
        free(json);
        if (!ok) {
            form_pairs_free(&pairs);
            return 1;
        }
    }
    for (int i = 0; i < nsets; i++) {
        const char *eq = strchr(sets[i], '=');
        if (!eq || eq == sets[i]) {
            fprintf(stderr, "tspdf form: invalid --set '%s' (want name=value)\n",
                    sets[i]);
            form_pairs_free(&pairs);
            return 1;
        }
        char *key = strndup(sets[i], (size_t)(eq - sets[i]));
        char *val = strdup(eq + 1);
        if (!key || !val || !form_pairs_add(&pairs, key, val)) {
            free(key);
            free(val);
            fprintf(stderr, "tspdf form: out of memory\n");
            form_pairs_free(&pairs);
            return 1;
        }
    }

    TspdfReader *doc = form_open(input, password);
    if (!doc) {
        form_pairs_free(&pairs);
        return 1;
    }

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_form_fields(doc, &fields, &count);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf form: cannot read form fields: %s\n",
                tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        form_pairs_free(&pairs);
        return 1;
    }

    // Reject the whole batch when any name is unknown, listing all of them.
    size_t unknown = 0;
    for (size_t i = 0; i < pairs.count; i++) {
        if (!form_find_field(fields, count, pairs.keys[i])) {
            fprintf(stderr, unknown == 0
                        ? "tspdf form: no such field: '%s'"
                        : ", '%s'",
                    pairs.keys[i]);
            unknown++;
        }
    }
    if (unknown > 0) {
        fprintf(stderr, " (run 'tspdf form list' to see field names)\n");
        tspdf_reader_destroy(doc);
        form_pairs_free(&pairs);
        return 1;
    }

    int rc = 0;
    for (size_t i = 0; i < pairs.count; i++) {
        const TspdfFormFieldInfo *f = form_find_field(fields, count,
                                                      pairs.keys[i]);
        const char *value = form_map_button_value(f, pairs.vals[i]);
        err = tspdf_reader_form_fill(doc, pairs.keys[i], value, force);
        if (err == TSPDF_ERR_UNSUPPORTED && f->readonly && !force) {
            fprintf(stderr, "tspdf form: field '%s' is readonly "
                    "(use --force to fill it anyway)\n", pairs.keys[i]);
            rc = 1;
            break;
        }
        if (err == TSPDF_ERR_INVALID_ARG &&
            (f->type == TSPDF_FIELD_CHECKBOX || f->type == TSPDF_FIELD_RADIO)) {
            fprintf(stderr, "tspdf form: '%s' is not a state of '%s' "
                    "(options: ", pairs.vals[i], pairs.keys[i]);
            for (size_t j = 0; j < f->option_count; j++) {
                fprintf(stderr, "%s%s", j > 0 ? ", " : "", f->options[j]);
            }
            fprintf(stderr, "%sOff)\n", f->option_count > 0 ? ", " : "");
            rc = 1;
            break;
        }
        if (err == TSPDF_ERR_INVALID_ARG && f->type == TSPDF_FIELD_CHOICE) {
            fprintf(stderr, "tspdf form: '%s' is not an option of '%s' "
                    "(options: ", pairs.vals[i], pairs.keys[i]);
            for (size_t j = 0; j < f->option_count; j++) {
                fprintf(stderr, "%s%s", j > 0 ? ", " : "", f->options[j]);
            }
            fprintf(stderr, ")\n");
            rc = 1;
            break;
        }
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf form: cannot fill '%s': %s\n",
                    pairs.keys[i], tspdf_error_string(err));
            rc = 1;
            break;
        }
        // The generated appearance stream (text fields) uses a cp1252 base
        // font with a fallback-font escape hatch: warn when characters are
        // lost anyway. /V itself is unaffected.
        if (f->type == TSPDF_FIELD_TEXT) {
            form_warn_lossy_value(doc, pairs.keys[i], value);
        }
    }

    if (rc == 0) {
        err = tspdf_reader_save(doc, output);
        if (err != TSPDF_OK) {
            fprintf(stderr, "tspdf form: failed to save '%s': %s\n", output,
                    tspdf_error_string(err));
            rc = 1;
        }
    }
    tspdf_reader_destroy(doc);
    form_pairs_free(&pairs);
    return rc;
}

// --- flatten ---

static int form_flatten(const char *input, const char *output,
                        const char *password) {
    if (!output) {
        fprintf(stderr, "tspdf form: missing output file (-o)\n");
        return 1;
    }
    TspdfReader *doc = form_open(input, password);
    if (!doc) return 1;

    // Flatten stamps current values with a cp1252 base font (fallback
    // TrueType font for characters outside it): warn up front about values
    // that will still lose characters in the stamped page content.
    {
        TspdfFormFieldInfo *fields = NULL;
        size_t count = 0;
        if (tspdf_reader_form_fields(doc, &fields, &count) == TSPDF_OK) {
            for (size_t i = 0; i < count; i++) {
                if ((fields[i].type == TSPDF_FIELD_TEXT ||
                     fields[i].type == TSPDF_FIELD_CHOICE) &&
                    fields[i].value) {
                    form_warn_lossy_value(doc, fields[i].name,
                                          fields[i].value);
                }
            }
        }
    }

    TspdfError err = tspdf_reader_form_flatten(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf form: flatten failed: %s\n",
                tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }
    err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf form: failed to save '%s': %s\n", output,
                tspdf_error_string(err));
        return 1;
    }
    return 0;
}

// --- entry point ---

int cmd_form(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        form_usage();
        return argc == 0 ? 1 : 0;
    }

    const char *sub = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    const char *positional[2];
    int npos = collect_positional(sub_argc, sub_argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf form: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf form: unexpected extra argument '%s'\n",
                positional[1]);
        return 1;
    }
    const char *input = positional[0];
    const char *output = find_flag(sub_argc, sub_argv, "-o");
    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(sub_argc, sub_argv,
                                                  "--password", "--password-file",
                                                  "form", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));

    if (strcmp(sub, "list") == 0) return form_list(input, password);
    if (strcmp(sub, "fill") == 0) {
        return form_fill(sub_argc, sub_argv, input, output, password);
    }
    if (strcmp(sub, "flatten") == 0) return form_flatten(input, output, password);

    fprintf(stderr, "tspdf form: unknown subcommand '%s' "
            "(expected list, fill, or flatten)\n", sub);
    return 1;
}
