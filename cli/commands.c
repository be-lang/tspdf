#include "commands.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

size_t *parse_page_range(const char *spec, size_t *out_count) {
    if (!spec || !out_count || *spec == '\0') return NULL;

    // First pass: count how many page numbers we'll produce
    size_t capacity = 16;
    size_t count = 0;
    size_t *pages = malloc(capacity * sizeof(size_t));
    if (!pages) return NULL;

    const char *p = spec;
    while (*p) {
        // Skip whitespace
        while (*p == ' ') p++;
        if (*p == '\0') break;

        // Parse a number
        if (!isdigit((unsigned char)*p)) {
            free(pages);
            return NULL;
        }
        char *end;
        long a = strtol(p, &end, 10);
        if (end == p || a < 1) {
            free(pages);
            return NULL;
        }
        p = end;

        long b = a;
        if (*p == '-') {
            p++;
            if (!isdigit((unsigned char)*p)) {
                free(pages);
                return NULL;
            }
            b = strtol(p, &end, 10);
            if (end == p || b < a) {
                free(pages);
                return NULL;
            }
            p = end;
        }

        // Emit pages a..b (1-based → 0-based)
        for (long i = a; i <= b; i++) {
            if (count >= capacity) {
                capacity *= 2;
                size_t *tmp = realloc(pages, capacity * sizeof(size_t));
                if (!tmp) {
                    free(pages);
                    return NULL;
                }
                pages = tmp;
            }
            pages[count++] = (size_t)(i - 1);
        }

        // Expect comma or end
        while (*p == ' ') p++;
        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            free(pages);
            return NULL;
        }
    }

    if (count == 0) {
        free(pages);
        return NULL;
    }

    *out_count = count;
    return pages;
}

const char *find_flag(int argc, char **argv, const char *flag) {
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

bool has_flag(int argc, char **argv, const char *flag) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int find_flags(int argc, char **argv, const char *flag, const char **out, int out_max) {
    int count = 0;
    for (int i = 0; i < argc - 1 && count < out_max; i++) {
        if (strcmp(argv[i], flag) == 0) {
            out[count++] = argv[i + 1];
        }
    }
    return count;
}

int collect_positional(int argc, char **argv, const char **out, int out_max) {
    int count = 0;
    for (int i = 0; i < argc && count < out_max; i++) {
        if (argv[i][0] != '-') {
            out[count++] = argv[i];
        }
    }
    return count;
}
