// System-font discovery for the form-fill fallback font. See font_fallback.h
// for the lookup contract. Everything here is best-effort: any parse or I/O
// failure just moves on to the next candidate, and a NULL result means the
// caller keeps its existing (lossy '?') rendering path.

#include "font_fallback.h"
#include "ttf_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <sys/stat.h>
#endif

// Bounds for the recursive scan: fonts directories are user-controlled and
// can be symlink-looped or enormous, so cap both depth and candidate count.
#define FB_MAX_DEPTH 6
#define FB_MAX_CANDIDATES 2048

static char *fb_strdup(const char *s) {
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (out) memcpy(out, s, len + 1);
    return out;
}

// Case-insensitive ASCII substring search (strcasestr is not portable).
static bool fb_contains_ci(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool fb_has_ext_ci(const char *name, const char *ext) {
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return false;
    return fb_contains_ci(name + nlen - elen, ext) &&
           strlen(name + nlen - elen) == elen;
}

int tspdf_fallback_font_score(const char *filename, bool want_cjk) {
    int score = 0;
    // .otf candidates are usually CFF-flavored (rejected by the coverage
    // check anyway); parse them last so common cases never pay for it.
    if (fb_has_ext_ci(filename, ".ttf") || fb_has_ext_ci(filename, ".ttc")) {
        score += 100;
    }
    if (fb_contains_ci(filename, "cjk")) score += want_cjk ? 1000 : -50;
    if (want_cjk) {
        // Common CJK family names, so machines that ship them as e.g.
        // "SourceHanSans" or "wqy-zenhei" still order them first.
        if (fb_contains_ci(filename, "han")) score += 300;
        if (fb_contains_ci(filename, "hei")) score += 200;
        if (fb_contains_ci(filename, "gothic")) score += 200;
        if (fb_contains_ci(filename, "mincho")) score += 200;
    }
    if (fb_contains_ci(filename, "noto")) score += 80;
    if (fb_contains_ci(filename, "dejavu")) score += 80;
    if (fb_contains_ci(filename, "droid")) score += 40;
    if (fb_contains_ci(filename, "liberation")) score += 40;
    if (fb_contains_ci(filename, "sans")) score += 30;
    if (fb_contains_ci(filename, "regular")) score += 30;
    if (fb_contains_ci(filename, "mono")) score -= 10;
    if (fb_contains_ci(filename, "bold")) score -= 30;
    if (fb_contains_ci(filename, "italic")) score -= 40;
    if (fb_contains_ci(filename, "oblique")) score -= 40;
    return score;
}

bool tspdf_fallback_font_covers(const char *path, const uint32_t *cps,
                                size_t count) {
    TTF_Font font;
    if (!ttf_load(&font, path)) return false;
    // The subsetter needs glyf outlines; CFF ('OTTO') fonts parse for
    // metrics but carry no glyf/loca, so they are rejected here.
    bool ok = font.glyf_offset != 0 && font.loca_offset != 0;
    for (size_t i = 0; ok && i < count; i++) {
        if (ttf_get_glyph_index(&font, cps[i]) == 0) ok = false;
    }
    ttf_free(&font);
    return ok;
}

// --- candidate collection ---

typedef struct {
    char *path;
    int score;
    size_t order;  // scan order, ties broken deterministically
} FbCandidate;

typedef struct {
    FbCandidate *items;
    size_t count;
    size_t cap;
} FbList;

static void fb_list_free(FbList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->items[i].path);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void fb_list_add(FbList *list, const char *path, int score) {
    if (list->count >= FB_MAX_CANDIDATES) return;
    if (list->count >= list->cap) {
        size_t cap = list->cap == 0 ? 64 : list->cap * 2;
        FbCandidate *grown =
            (FbCandidate *)realloc(list->items, cap * sizeof(FbCandidate));
        if (!grown) return;
        list->items = grown;
        list->cap = cap;
    }
    char *copy = fb_strdup(path);
    if (!copy) return;
    list->items[list->count].path = copy;
    list->items[list->count].score = score;
    list->items[list->count].order = list->count;
    list->count++;
}

static int fb_candidate_cmp(const void *a, const void *b) {
    const FbCandidate *ca = (const FbCandidate *)a;
    const FbCandidate *cb = (const FbCandidate *)b;
    if (ca->score != cb->score) return cb->score - ca->score;
    // Deterministic tie-break: keep scan order stable across qsort impls.
    return ca->order < cb->order ? -1 : (ca->order > cb->order ? 1 : 0);
}

#if !defined(_WIN32)

static void fb_scan_dir(FbList *list, const char *dir, bool want_cjk,
                        int depth) {
    if (depth > FB_MAX_DEPTH || list->count >= FB_MAX_CANDIDATES) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && list->count < FB_MAX_CANDIDATES) {
        if (ent->d_name[0] == '.') continue;  // ., .., hidden
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            fb_scan_dir(list, path, want_cjk, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            if (fb_has_ext_ci(ent->d_name, ".ttf") ||
                fb_has_ext_ci(ent->d_name, ".ttc") ||
                fb_has_ext_ci(ent->d_name, ".otf")) {
                fb_list_add(list, path,
                            tspdf_fallback_font_score(ent->d_name, want_cjk));
            }
        }
    }
    closedir(d);
}

static void fb_scan_roots(FbList *list, bool want_cjk) {
    const char *dirs_env = getenv("TSPDF_FONT_DIRS");
    if (dirs_env && dirs_env[0]) {
        // Colon-separated override of the built-in roots (also makes the
        // discovery hermetic under test).
        char *dirs = fb_strdup(dirs_env);
        if (!dirs) return;
        char *save = dirs;
        while (save && *save) {
            char *colon = strchr(save, ':');
            if (colon) *colon = '\0';
            if (*save) fb_scan_dir(list, save, want_cjk, 0);
            save = colon ? colon + 1 : NULL;
        }
        free(dirs);
        return;
    }
    fb_scan_dir(list, "/usr/share/fonts", want_cjk, 0);
    fb_scan_dir(list, "/usr/local/share/fonts", want_cjk, 0);
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/.fonts", home);
        if (n > 0 && (size_t)n < sizeof(path)) {
            fb_scan_dir(list, path, want_cjk, 0);
        }
        n = snprintf(path, sizeof(path), "%s/.local/share/fonts", home);
        if (n > 0 && (size_t)n < sizeof(path)) {
            fb_scan_dir(list, path, want_cjk, 0);
        }
    }
    // macOS system locations (harmlessly absent elsewhere).
    fb_scan_dir(list, "/System/Library/Fonts", want_cjk, 0);
    fb_scan_dir(list, "/Library/Fonts", want_cjk, 0);
}

#else  // _WIN32: no directory scan; the env override still works.

static void fb_scan_roots(FbList *list, bool want_cjk) {
    (void)list;
    (void)want_cjk;
}

#endif

char *tspdf_fallback_font_find(const uint32_t *cps, size_t count) {
    // Explicit override: use it or fail, never scan behind the user's back
    // (that would make "why did it pick that font" undebuggable).
    const char *env = getenv("TSPDF_FALLBACK_FONT");
    if (env && env[0]) {
        if (tspdf_fallback_font_covers(env, cps, count)) return fb_strdup(env);
        return NULL;
    }

    bool want_cjk = false;
    for (size_t i = 0; i < count; i++) {
        if (cps[i] >= 0x2E80 && cps[i] <= 0x9FFF) want_cjk = true;   // CJK+kana
        if (cps[i] >= 0xF900 && cps[i] <= 0xFAFF) want_cjk = true;   // compat
        if (cps[i] >= 0x20000 && cps[i] <= 0x2FA1F) want_cjk = true; // ext B+
        if (cps[i] >= 0xAC00 && cps[i] <= 0xD7AF) want_cjk = true;   // hangul
    }

    FbList list = {0};
    fb_scan_roots(&list, want_cjk);
    if (list.count == 0) {
        fb_list_free(&list);
        return NULL;
    }
    qsort(list.items, list.count, sizeof(FbCandidate), fb_candidate_cmp);

    char *result = NULL;
    for (size_t i = 0; i < list.count && !result; i++) {
        if (tspdf_fallback_font_covers(list.items[i].path, cps, count)) {
            result = fb_strdup(list.items[i].path);
        }
    }
    fb_list_free(&list);
    return result;
}
