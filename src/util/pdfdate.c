#include "pdfdate.h"

#include <stdio.h>

/* Parse exactly `n` digits at *p, advancing it. Returns -1 if any byte in
 * the window is not a digit (including a premature end of string). */
static int take_digits(const char **p, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    *p += n;
    return value;
}

bool tspdf_format_pdf_date(const char *raw, char *out, size_t out_size) {
    if (!raw || !out || out_size == 0) return false;

    const char *p = raw;
    if (p[0] == 'D' && p[1] == ':') p += 2;

    int year = take_digits(&p, 4);
    if (year < 0) return false;

    /* Month through second are each optional but positional: a field may
     * only appear when every earlier field is present. */
    int month = 1, day = 1, hour = 0, minute = 0, second = 0;
    int *fields[] = {&month, &day, &hour, &minute, &second};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (*p == '\0' || *p == 'Z' || *p == '+' || *p == '-') break;
        int v = take_digits(&p, 2);
        if (v < 0) return false;
        *fields[i] = v;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    /* Zone: Z, or +/-HH'mm'. Adobe writes +04'00', but real files also
     * drop the trailing apostrophe, the minutes, or both apostrophes
     * (+0400), so each piece after the hours is optional. */
    char zone[16] = "";
    if (*p == 'Z') {
        p++;
        if (*p == '\0') snprintf(zone, sizeof(zone), " UTC");
        else return false;
    } else if (*p == '+' || *p == '-') {
        char sign = *p++;
        int zh = take_digits(&p, 2);
        if (zh < 0 || zh > 23) return false;
        int zm = 0;
        if (*p == '\'') p++;
        if (*p != '\0') {
            zm = take_digits(&p, 2);
            if (zm < 0 || zm > 59) return false;
            if (*p == '\'') p++;
        }
        if (*p != '\0') return false;
        snprintf(zone, sizeof(zone), " %c%02d:%02d", sign, zh, zm);
    } else if (*p != '\0') {
        return false;
    }

    int n = snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d%s",
                     year, month, day, hour, minute, second, zone);
    return n > 0 && (size_t)n < out_size;
}
