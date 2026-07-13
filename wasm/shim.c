// wasm/shim.c — C ABI the browser (and the node test harness) calls.
//
// Conventions:
//   * tspdf_wasm_open takes ownership of `data` (malloc'd on the wasm heap by
//     the caller) and frees it when the handle is closed. The reader keeps
//     referencing the buffer, so the caller must NOT free it itself.
//   * Functions returning uint8_t*/char* return a malloc'd buffer; release it
//     with tspdf_wasm_free_result(). On failure they return NULL and
//     tspdf_wasm_last_error() describes why.
//   * Page/order arrays are zero-based uint32_t indices (HEAPU32 on the JS
//     side); out_len is written through a uint32_t* the JS side reads back.
//   * Only reader-side operations are exposed: the ones the web UI offers
//     that map cleanly onto the reader API. Writer-side tools (img2pdf,
//     qrcode, md2pdf) are intentionally absent from the v1 wasm build.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../src/reader/tspr.h"
#include "../include/tspdf/version.h"
#include "../src/ops/ops.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

#define WASM_MAX_DOCS 64

typedef struct {
    TspdfReader *doc;
    uint8_t *data;  // heap buffer the reader references; owned by the handle
} WasmDoc;

static WasmDoc g_docs[WASM_MAX_DOCS];
static char g_error[256];

static void wasm_set_error(const char *msg) {
    snprintf(g_error, sizeof(g_error), "%s", msg ? msg : "unknown error");
}

static void wasm_set_error_code(TspdfError err) {
    wasm_set_error(tspdf_error_string(err));
}

static TspdfReader *wasm_get(int h) {
    if (h < 1 || h > WASM_MAX_DOCS || !g_docs[h - 1].doc) {
        wasm_set_error("invalid document handle");
        return NULL;
    }
    return g_docs[h - 1].doc;
}

// Save `doc` to a malloc'd buffer; NULL + error message on failure.
static uint8_t *wasm_save_doc(TspdfReader *doc, uint32_t *out_len) {
    uint8_t *out = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory(doc, &out, &len);
    if (err != TSPDF_OK) {
        free(out);
        wasm_set_error_code(err);
        return NULL;
    }
    *out_len = (uint32_t)len;
    return out;
}

WASM_EXPORT const char *tspdf_wasm_version(void) {
    return TSPDF_VERSION_STRING;
}

WASM_EXPORT const char *tspdf_wasm_last_error(void) {
    return g_error;
}

WASM_EXPORT void tspdf_wasm_free_result(uint8_t *p) {
    free(p);
}

// Open a PDF from `data` (ownership transfers to the handle). `password` may
// be NULL for unencrypted documents. Returns handle > 0, or 0 on failure
// (in which case `data` is freed here — the caller must not reuse it).
WASM_EXPORT int tspdf_wasm_open(uint8_t *data, uint32_t len, const char *password) {
    int slot = -1;
    for (int i = 0; i < WASM_MAX_DOCS; i++) {
        if (!g_docs[i].doc) { slot = i; break; }
    }
    if (slot < 0) {
        free(data);
        wasm_set_error("too many open documents");
        return 0;
    }

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = password
        ? tspdf_reader_open_with_password(data, len, password, &err)
        : tspdf_reader_open(data, len, &err);
    if (!doc) {
        free(data);
        wasm_set_error_code(err);
        return 0;
    }
    g_docs[slot].doc = doc;
    g_docs[slot].data = data;
    return slot + 1;
}

WASM_EXPORT void tspdf_wasm_close(int h) {
    if (h < 1 || h > WASM_MAX_DOCS) return;
    WasmDoc *wd = &g_docs[h - 1];
    if (wd->doc) tspdf_reader_destroy(wd->doc);
    free(wd->data);
    wd->doc = NULL;
    wd->data = NULL;
}

WASM_EXPORT int tspdf_wasm_page_count(int h) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return -1;
    return (int)tspdf_reader_page_count(doc);
}

WASM_EXPORT uint8_t *tspdf_wasm_save(int h, uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    return wasm_save_doc(doc, out_len);
}

// Save with the encryption removed. A plain save preserves a password-opened
// document's encryption, so the unlock tool needs this explicit opt-out
// (mirroring `tspdf decrypt` and the server's unlock endpoint). Thin adapter
// over tsops_unlock_save_options so the three callers (CLI, server, wasm)
// share one implementation.
WASM_EXPORT uint8_t *tspdf_wasm_save_decrypted(int h, uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    TspdfSaveOptions opts = tsops_unlock_save_options();
    uint8_t *out = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(doc, &out, &len, &opts);
    if (err != TSPDF_OK) {
        free(out);
        wasm_set_error_code(err);
        return NULL;
    }
    *out_len = (uint32_t)len;
    return out;
}

WASM_EXPORT uint8_t *tspdf_wasm_merge(const int *handles, uint32_t n, uint32_t *out_len) {
    if (n < 2) { wasm_set_error("merge needs at least 2 documents"); return NULL; }
    TspdfReader *docs[WASM_MAX_DOCS];
    if (n > WASM_MAX_DOCS) { wasm_set_error("too many documents"); return NULL; }
    for (uint32_t i = 0; i < n; i++) {
        docs[i] = wasm_get(handles[i]);
        if (!docs[i]) return NULL;
    }
    TspdfError err = TSPDF_OK;
    TspdfReader *merged = tspdf_reader_merge(docs, n, &err);
    if (!merged) { wasm_set_error_code(err); return NULL; }
    uint8_t *out = wasm_save_doc(merged, out_len);
    tspdf_reader_destroy(merged);
    return out;
}

// Shared page-list op runner: derive a new doc, save it, destroy it.
typedef TspdfReader *(*wasm_pages_op)(TspdfReader *, const size_t *, size_t, TspdfError *);

static uint8_t *wasm_run_pages_op(wasm_pages_op op, int h,
                                  const uint32_t *pages, uint32_t n,
                                  uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    size_t *indices = malloc(n ? n * sizeof(size_t) : sizeof(size_t));
    if (!indices) { wasm_set_error("out of memory"); return NULL; }
    for (uint32_t i = 0; i < n; i++) indices[i] = pages[i];
    TspdfError err = TSPDF_OK;
    TspdfReader *result = op(doc, indices, n, &err);
    free(indices);
    if (!result) { wasm_set_error_code(err); return NULL; }
    uint8_t *out = wasm_save_doc(result, out_len);
    tspdf_reader_destroy(result);
    return out;
}

WASM_EXPORT uint8_t *tspdf_wasm_extract(int h, const uint32_t *pages, uint32_t n,
                                        uint32_t *out_len) {
    return wasm_run_pages_op(tspdf_reader_extract, h, pages, n, out_len);
}

WASM_EXPORT uint8_t *tspdf_wasm_delete(int h, const uint32_t *pages, uint32_t n,
                                       uint32_t *out_len) {
    return wasm_run_pages_op(tspdf_reader_delete, h, pages, n, out_len);
}

WASM_EXPORT uint8_t *tspdf_wasm_reorder(int h, const uint32_t *order, uint32_t n,
                                        uint32_t *out_len) {
    return wasm_run_pages_op(tspdf_reader_reorder, h, order, n, out_len);
}

// `n == 0` rotates every page (mirrors the server's "all" behaviour).
WASM_EXPORT uint8_t *tspdf_wasm_rotate(int h, const uint32_t *pages, uint32_t n,
                                       int angle, uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    size_t count = n ? n : tspdf_reader_page_count(doc);
    size_t *indices = malloc(count ? count * sizeof(size_t) : sizeof(size_t));
    if (!indices) { wasm_set_error("out of memory"); return NULL; }
    if (n) {
        for (uint32_t i = 0; i < n; i++) indices[i] = pages[i];
    } else {
        for (size_t i = 0; i < count; i++) indices[i] = i;
    }
    TspdfError err = TSPDF_OK;
    TspdfReader *result = tspdf_reader_rotate(doc, indices, count, angle, &err);
    free(indices);
    if (!result) { wasm_set_error_code(err); return NULL; }
    uint8_t *out = wasm_save_doc(result, out_len);
    tspdf_reader_destroy(result);
    return out;
}

WASM_EXPORT uint8_t *tspdf_wasm_compress(int h, int strip_unused, int recompress,
                                         uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = strip_unused != 0;
    opts.recompress_streams = recompress != 0;
    uint8_t *out = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory_with_options(doc, &out, &len, &opts);
    if (err != TSPDF_OK) { free(out); wasm_set_error_code(err); return NULL; }
    *out_len = (uint32_t)len;
    return out;
}

// key_bits: 128 (AES-128/V4R4) or 256 (AES-256/V5R6); permissions all-on,
// matching the server's password-protect endpoint.
WASM_EXPORT uint8_t *tspdf_wasm_encrypt(int h, const char *user_pass,
                                        const char *owner_pass, int key_bits,
                                        uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    if (key_bits != 128 && key_bits != 256) key_bits = 128;
    uint8_t *out = NULL;
    size_t len = 0;
    TspdfError err = tspdf_reader_save_to_memory_encrypted(doc, &out, &len,
                                                           user_pass, owner_pass,
                                                           0xFFFFFFFF, key_bits);
    if (err != TSPDF_OK) { free(out); wasm_set_error_code(err); return NULL; }
    *out_len = (uint32_t)len;
    return out;
}

// Diagonal text watermark on every page (same drawing as the CLI's `tspdf
// watermark` and the server's watermark endpoint). Thin adapter over
// tsops_watermark_text — which correctly compensates page /Rotate so the
// watermark reads upright on rotated pages. Previously this function had its
// own copy of the drawing loop that ignored /Rotate; now all three callers
// share a single implementation and the bug is fixed.
// Mutates the handle's document, then saves it.
WASM_EXPORT uint8_t *tspdf_wasm_watermark_text(int h, const char *text,
                                               double font_size, double opacity,
                                               uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    if (!text || !text[0]) text = "DRAFT";
    if (font_size <= 0.0) font_size = 48.0;
    if (opacity <= 0.0 || opacity > 1.0) opacity = 0.3;

    TsopsWatermarkText params = {
        .text = text, .opacity = opacity, .font_size = font_size,
    };
    TspdfError err = tsops_watermark_text(doc, &params, NULL);
    if (err != TSPDF_OK) {
        wasm_set_error_code(err);
        return NULL;
    }

    return wasm_save_doc(doc, out_len);
}

// Metadata setters: NULL leaves a field unchanged (pass "" to clear it, as
// the web UI does). Mutates the handle's document, then saves it.
WASM_EXPORT uint8_t *tspdf_wasm_set_metadata(int h, const char *title,
                                             const char *author,
                                             const char *subject,
                                             const char *keywords,
                                             uint32_t *out_len) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;
    if (title) tspdf_reader_set_title(doc, title);
    if (author) tspdf_reader_set_author(doc, author);
    if (subject) tspdf_reader_set_subject(doc, subject);
    if (keywords) tspdf_reader_set_keywords(doc, keywords);
    return wasm_save_doc(doc, out_len);
}

// --- Metadata as JSON (for the metadata-view tool) ---

typedef struct {
    char *data;
    size_t len, cap;
} JsonBuf;

static int jb_put(JsonBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 256);
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int jb_str(JsonBuf *b, const char *s) {
    if (!jb_put(b, "\"", 1)) return 0;
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\'; esc[1] = (char)c;
            if (!jb_put(b, esc, 2)) return 0;
        } else if (c < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            if (!jb_put(b, esc, 6)) return 0;
        } else {
            if (!jb_put(b, (const char *)p, 1)) return 0;
        }
    }
    return jb_put(b, "\"", 1);
}

// JSON object with the same fields as the server's /api/metadata-view.
// Returned string is malloc'd; free with tspdf_wasm_free_result.
WASM_EXPORT char *tspdf_wasm_metadata_json(int h) {
    TspdfReader *doc = wasm_get(h);
    if (!doc) return NULL;

    const char *fields[][2] = {
        { "title",    tspdf_reader_get_title(doc) },
        { "author",   tspdf_reader_get_author(doc) },
        { "subject",  tspdf_reader_get_subject(doc) },
        { "keywords", tspdf_reader_get_keywords(doc) },
        { "creator",  tspdf_reader_get_creator(doc) },
        { "producer", tspdf_reader_get_producer(doc) },
        { "created",  tspdf_reader_get_creation_date(doc) },
        { "modified", tspdf_reader_get_mod_date(doc) },
    };

    JsonBuf b = {0};
    if (!jb_put(&b, "{", 1)) goto fail;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i && !jb_put(&b, ",", 1)) goto fail;
        if (!jb_str(&b, fields[i][0])) goto fail;
        if (!jb_put(&b, ":", 1)) goto fail;
        if (!jb_str(&b, fields[i][1] ? fields[i][1] : "")) goto fail;
    }
    char pages[48];
    int n = snprintf(pages, sizeof(pages), ",\"pages\":%u}",
                     (unsigned)tspdf_reader_page_count(doc));
    if (n <= 0 || !jb_put(&b, pages, (size_t)n)) goto fail;
    return b.data;

fail:
    free(b.data);
    wasm_set_error("out of memory");
    return NULL;
}
