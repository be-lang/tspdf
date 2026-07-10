// tspdf bookmark — list, add, import and clear PDF outlines (bookmarks).
//
// TOC file format (import):
//   One entry per line, TAB-separated:  LEVEL <TAB> PAGE <TAB> TITLE
//     LEVEL  1-based nesting depth (1 = top level; must not jump by >1)
//     PAGE   1-based page number
//     TITLE  bookmark text (UTF-8; may contain spaces, not tabs)
//   Blank lines and lines whose first non-space character is '#' are ignored.
// This is exactly what `bookmark list` prints (without --json), so a listing
// can be edited and fed straight back with `bookmark import`.

#include "commands.h"
#include "password_input.h"
#include "../include/tspdf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BM_MAX_ENTRIES 100000

static void bookmark_usage(void) {
    printf("Usage: tspdf bookmark list <input.pdf> [--json]\n");
    printf("       tspdf bookmark add <input.pdf> --title <text> --page <N> [--level <L>] -o <output.pdf>\n");
    printf("       tspdf bookmark import <input.pdf> --from <toc.txt|-> [--append] -o <output.pdf>\n");
    printf("       tspdf bookmark clear <input.pdf> -o <output.pdf>\n");
    printf("\n");
    printf("Edit the outline (bookmarks) of an existing PDF.\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  list --json               Machine-readable listing (title/level/page)\n");
    printf("  add --title <text>        Bookmark title to append\n");
    printf("  add --page <N>            Target page (1-based)\n");
    printf("  add --level <L>           Nesting level (1-based, default 1)\n");
    printf("  import --from <file>      TOC file (LEVEL<TAB>PAGE<TAB>TITLE per line; - is stdin)\n");
    printf("  import --append           Append the TOC after the existing outline instead of replacing it\n");
    printf("  --password <pass>         Password for encrypted PDFs (or --password-file)\n");
    printf("  -o <output.pdf>           Output file (required for add/import/clear)\n");
}

// Open the input, resolving an optional --password/--password-file pair.
static TspdfReader *bookmark_open(int argc, char **argv, const char *input) {
    static char pwbuf[TSPDF_PASSWORD_MAX];
    const char *password = tspdf_resolve_password(argc, argv,
                                                  "--password", "--password-file",
                                                  "bookmark", "Password: ",
                                                  false, pwbuf, sizeof(pwbuf));
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_file_with_password(input, password, &err)
        : tspdf_reader_open_file(input, &err);
    if (!doc) {
        if (err == TSPDF_ERR_ENCRYPTED) {
            fprintf(stderr, "tspdf bookmark: '%s' is encrypted; use --password or "
                            "--password-file\n", input);
        } else {
            fprintf(stderr, "tspdf bookmark: failed to open '%s': %s\n",
                    input, tspdf_error_string(err));
        }
    }
    return doc;
}

static void bookmark_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20) printf("\\u%04X", *p);
                else putchar(*p);
                break;
        }
    }
    putchar('"');
}

static int bookmark_list(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf bookmark list: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf bookmark list: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    bool json = has_flag(argc, argv, "--json");

    TspdfReader *doc = bookmark_open(argc, argv, positional[0]);
    if (!doc) return 1;

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(doc, &bm, &n);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark list: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    if (json) {
        printf("[");
        for (size_t i = 0; i < n; i++) {
            printf("%s{\"title\":", i > 0 ? "," : "");
            bookmark_json_string(bm[i].title);
            printf(",\"level\":%d,\"page\":", bm[i].level);
            if (bm[i].page_index == (size_t)-1) fputs("null", stdout);
            else printf("%zu", bm[i].page_index + 1);
            printf(",\"open\":%s}", bm[i].open ? "true" : "false");
        }
        printf("]\n");
    } else {
        // LEVEL<TAB>PAGE<TAB>TITLE — the exact format `bookmark import` reads.
        // A leading indent of two spaces per extra level makes the tree
        // readable without breaking the TAB-delimited parse.
        for (size_t i = 0; i < n; i++) {
            if (bm[i].page_index == (size_t)-1) {
                printf("%d\t-\t%s\n", bm[i].level, bm[i].title);
            } else {
                printf("%d\t%zu\t%s\n", bm[i].level, bm[i].page_index + 1, bm[i].title);
            }
        }
    }

    tspdf_reader_destroy(doc);
    return 0;
}

static int bookmark_add(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf bookmark add: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf bookmark add: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *title = find_flag(argc, argv, "--title");
    const char *page_s = find_flag(argc, argv, "--page");
    const char *level_s = find_flag(argc, argv, "--level");
    const char *output = find_flag(argc, argv, "-o");
    if (!title || title[0] == '\0') {
        fprintf(stderr, "tspdf bookmark add: missing --title <text>\n");
        return 1;
    }
    if (!page_s) {
        fprintf(stderr, "tspdf bookmark add: missing --page <N>\n");
        return 1;
    }
    if (!output) {
        fprintf(stderr, "tspdf bookmark add: missing -o <output.pdf>\n");
        return 1;
    }
    char *end = NULL;
    long page = strtol(page_s, &end, 10);
    if (*page_s == '\0' || *end != '\0' || page < 1) {
        fprintf(stderr, "tspdf bookmark add: --page must be a page number >= 1\n");
        return 1;
    }
    long level = 1;
    if (level_s) {
        level = strtol(level_s, &end, 10);
        if (*level_s == '\0' || *end != '\0' || level < 1) {
            fprintf(stderr, "tspdf bookmark add: --level must be >= 1\n");
            return 1;
        }
    }

    TspdfReader *doc = bookmark_open(argc, argv, positional[0]);
    if (!doc) return 1;

    // Read existing bookmarks, append one, and set the whole list back.
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    TspdfError err = tspdf_reader_bookmarks(doc, &bm, &n);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark add: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    // The appended level must not jump above the previous entry's level + 1
    // (or 1 for an empty outline).
    int prev_level = (n > 0) ? bm[n - 1].level : 0;
    if (level > prev_level + 1) {
        fprintf(stderr, "tspdf bookmark add: --level %ld jumps more than one below "
                        "the last entry (level %d)\n", level, prev_level);
        tspdf_reader_destroy(doc);
        return 1;
    }

    TspdfBookmarkEntry *entries =
        (TspdfBookmarkEntry *)calloc(n + 1, sizeof(TspdfBookmarkEntry));
    if (!entries) {
        fprintf(stderr, "tspdf bookmark add: out of memory\n");
        tspdf_reader_destroy(doc);
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
        entries[i].keep = bm[i].node;   // preserve dest/color/flags/collapse
    }
    entries[n].title = title;
    entries[n].level = (int)level;
    entries[n].page_index = (size_t)(page - 1);

    err = tspdf_reader_set_bookmarks(doc, entries, n + 1);
    free(entries);
    if (err != TSPDF_OK) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            fprintf(stderr, "tspdf bookmark add: page %ld out of range\n", page);
        } else {
            fprintf(stderr, "tspdf bookmark add: %s\n", tspdf_error_string(err));
        }
        tspdf_reader_destroy(doc);
        return 1;
    }

    err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark add: failed to save '%s': %s\n",
                output, tspdf_error_string(err));
        return 1;
    }
    printf("Added bookmark → %s\n", output);
    return 0;
}

// Read a whole stream (file or stdin) into a malloc'd, NUL-terminated buffer.
static char *bookmark_slurp(const char *path, size_t *out_len) {
    FILE *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { if (f != stdin) fclose(f); return NULL; }
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) {
            size_t ncap = cap * 2;
            char *grown = (char *)realloc(buf, ncap);
            if (!grown) { free(buf); if (f != stdin) fclose(f); return NULL; }
            buf = grown;
            cap = ncap;
        }
    }
    if (f != stdin) fclose(f);
    char *grown = (char *)realloc(buf, len + 1);
    if (grown) buf = grown;
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

// Trim trailing '\r' and spaces from a line (in place).
static void bookmark_rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static int bookmark_import(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf bookmark import: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf bookmark import: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *from = find_flag(argc, argv, "--from");
    const char *output = find_flag(argc, argv, "-o");
    if (!from) {
        fprintf(stderr, "tspdf bookmark import: missing --from <toc.txt|->\n");
        return 1;
    }
    if (!output) {
        fprintf(stderr, "tspdf bookmark import: missing -o <output.pdf>\n");
        return 1;
    }

    size_t toc_len = 0;
    char *toc = bookmark_slurp(from, &toc_len);
    if (!toc) {
        fprintf(stderr, "tspdf bookmark import: cannot read '%s'\n", from);
        return 1;
    }

    // Parse LEVEL<TAB>PAGE<TAB>TITLE lines into an entry array. Titles are
    // pointers into `toc`, so `toc` must outlive the set_bookmarks call.
    TspdfBookmarkEntry *entries = NULL;
    size_t count = 0, cap = 0;
    int rc = 0;
    int lineno = 0;
    // Split on '\n' by hand (strtok would coalesce blank lines and mangle the
    // line count used in error messages).
    char *cursor = toc;
    while (cursor && *cursor) {
        lineno++;
        char *line = cursor;
        char *nl = strchr(cursor, '\n');
        if (nl) { *nl = '\0'; cursor = nl + 1; }
        else cursor = NULL;

        // Skip leading whitespace to spot blank/comment lines.
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\0' || *p == '#') continue;

        // LEVEL up to first TAB.
        char *t1 = strchr(p, '\t');
        if (!t1) {
            fprintf(stderr, "tspdf bookmark import: line %d: expected "
                            "LEVEL<TAB>PAGE<TAB>TITLE\n", lineno);
            rc = 1; break;
        }
        *t1 = '\0';
        char *page_s = t1 + 1;
        char *t2 = strchr(page_s, '\t');
        if (!t2) {
            fprintf(stderr, "tspdf bookmark import: line %d: expected "
                            "LEVEL<TAB>PAGE<TAB>TITLE\n", lineno);
            rc = 1; break;
        }
        *t2 = '\0';
        char *title = t2 + 1;
        bookmark_rstrip(title);

        char *end = NULL;
        long level = strtol(p, &end, 10);
        while (*end == ' ') end++;
        if (*p == '\0' || *end != '\0' || level < 1) {
            fprintf(stderr, "tspdf bookmark import: line %d: bad level '%s'\n", lineno, p);
            rc = 1; break;
        }
        while (*page_s == ' ') page_s++;
        long page = strtol(page_s, &end, 10);
        while (*end == ' ') end++;
        if (*page_s == '\0' || *end != '\0' || page < 1) {
            fprintf(stderr, "tspdf bookmark import: line %d: bad page '%s'\n", lineno, page_s);
            rc = 1; break;
        }
        if (title[0] == '\0') {
            fprintf(stderr, "tspdf bookmark import: line %d: empty title\n", lineno);
            rc = 1; break;
        }
        if (count >= BM_MAX_ENTRIES) {
            fprintf(stderr, "tspdf bookmark import: too many entries (max %d)\n",
                    BM_MAX_ENTRIES);
            rc = 1; break;
        }
        if (count >= cap) {
            size_t ncap = cap == 0 ? 64 : cap * 2;
            TspdfBookmarkEntry *grown =
                (TspdfBookmarkEntry *)realloc(entries, ncap * sizeof(*entries));
            if (!grown) { fprintf(stderr, "tspdf bookmark import: out of memory\n"); rc = 1; break; }
            entries = grown;
            cap = ncap;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        entries[count].title = title;
        entries[count].level = (int)level;
        entries[count].page_index = (size_t)(page - 1);
        count++;
    }

    if (rc != 0) { free(entries); free(toc); return 1; }
    if (count == 0) {
        fprintf(stderr, "tspdf bookmark import: '%s' has no entries\n", from);
        free(entries); free(toc);
        return 1;
    }

    TspdfReader *doc = bookmark_open(argc, argv, positional[0]);
    if (!doc) { free(entries); free(toc); return 1; }

    size_t imported = count;

    // --append keeps the existing outline (destinations, colors, flags and
    // collapse state verbatim) and adds the imported entries after it.
    if (has_flag(argc, argv, "--append")) {
        TspdfBookmarkInfo *bm = NULL;
        size_t n = 0;
        TspdfError lerr = tspdf_reader_bookmarks(doc, &bm, &n);
        if (lerr != TSPDF_OK) {
            fprintf(stderr, "tspdf bookmark import: %s\n", tspdf_error_string(lerr));
            tspdf_reader_destroy(doc);
            free(entries); free(toc);
            return 1;
        }
        if (n > 0) {
            int last_level = bm[n - 1].level;
            if (entries[0].level > last_level + 1) {
                fprintf(stderr, "tspdf bookmark import: first imported entry "
                                "(level %d) jumps more than one below the last "
                                "existing entry (level %d)\n",
                        entries[0].level, last_level);
                tspdf_reader_destroy(doc);
                free(entries); free(toc);
                return 1;
            }
            TspdfBookmarkEntry *combined = (TspdfBookmarkEntry *)calloc(
                n + count, sizeof(TspdfBookmarkEntry));
            if (!combined) {
                fprintf(stderr, "tspdf bookmark import: out of memory\n");
                tspdf_reader_destroy(doc);
                free(entries); free(toc);
                return 1;
            }
            for (size_t i = 0; i < n; i++) {
                combined[i].title = bm[i].title;
                combined[i].level = bm[i].level;
                combined[i].page_index = bm[i].page_index;
                combined[i].keep = bm[i].node;
            }
            memcpy(combined + n, entries, count * sizeof(TspdfBookmarkEntry));
            free(entries);
            entries = combined;
            count += n;
        }
    }

    TspdfError err = tspdf_reader_set_bookmarks(doc, entries, count);
    if (err != TSPDF_OK) {
        if (err == TSPDF_ERR_PAGE_RANGE) {
            fprintf(stderr, "tspdf bookmark import: a page number is out of range "
                            "(document has %zu pages)\n",
                    tspdf_reader_page_count(doc));
        } else if (err == TSPDF_ERR_INVALID_ARG) {
            fprintf(stderr, "tspdf bookmark import: invalid outline — the first "
                            "entry must be level 1 and levels must not jump by "
                            "more than one\n");
        } else {
            fprintf(stderr, "tspdf bookmark import: %s\n", tspdf_error_string(err));
        }
        tspdf_reader_destroy(doc);
        free(entries); free(toc);
        return 1;
    }
    free(entries);
    free(toc);

    err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark import: failed to save '%s': %s\n",
                output, tspdf_error_string(err));
        return 1;
    }
    printf("Imported %zu bookmark(s) → %s\n", imported, output);
    return 0;
}

static int bookmark_clear(int argc, char **argv) {
    const char *positional[2];
    int npos = collect_positional(argc, argv, positional, 2);
    if (npos < 1) {
        fprintf(stderr, "tspdf bookmark clear: missing input file\n");
        return 1;
    }
    if (npos > 1) {
        fprintf(stderr, "tspdf bookmark clear: unexpected extra argument '%s'\n", positional[1]);
        return 1;
    }
    const char *output = find_flag(argc, argv, "-o");
    if (!output) {
        fprintf(stderr, "tspdf bookmark clear: missing -o <output.pdf>\n");
        return 1;
    }

    TspdfReader *doc = bookmark_open(argc, argv, positional[0]);
    if (!doc) return 1;

    TspdfError err = tspdf_reader_clear_bookmarks(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark clear: %s\n", tspdf_error_string(err));
        tspdf_reader_destroy(doc);
        return 1;
    }

    err = tspdf_reader_save(doc, output);
    tspdf_reader_destroy(doc);
    if (err != TSPDF_OK) {
        fprintf(stderr, "tspdf bookmark clear: failed to save '%s': %s\n",
                output, tspdf_error_string(err));
        return 1;
    }
    printf("Cleared bookmarks → %s\n", output);
    return 0;
}

int cmd_bookmark(int argc, char **argv) {
    if (argc == 0 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        bookmark_usage();
        return argc == 0 ? 1 : 0;
    }

    const char *verb = argv[0];
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (strcmp(verb, "list") == 0)   return bookmark_list(sub_argc, sub_argv);
    if (strcmp(verb, "add") == 0)    return bookmark_add(sub_argc, sub_argv);
    if (strcmp(verb, "import") == 0) return bookmark_import(sub_argc, sub_argv);
    if (strcmp(verb, "clear") == 0)  return bookmark_clear(sub_argc, sub_argv);

    fprintf(stderr, "tspdf bookmark: unknown subcommand '%s'\n\n", verb);
    bookmark_usage();
    return 1;
}
