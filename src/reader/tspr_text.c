// Content-stream text extraction (pdftotext-style, digitally-born PDFs).
//
// Decoding priority per font: /ToUnicode CMap (bfchar + bfrange, incl. the
// array form), then /Encoding name + /Differences (glyph names via a compact
// AGL subset), then StandardEncoding. Type0 fonts use 2-byte codes; without
// /ToUnicode each glyph becomes U+FFFD (counted in TspdfTextStats).
//
// Layout is heuristic and stays in content-stream order: a baseline jump
// beyond 0.3 em emits a newline, a large x-gap within a line emits a space,
// and TJ adjustments beyond ~half the space width emit a space. Robustness
// beats fidelity: unknown operators are skipped, malformed CMaps degrade to
// the next fallback, and nothing here may crash on hostile input.

#include "tspr_internal.h"
#include "tspr_text.h"
#include "../util/pdftext.h"
#include "../util/buffer.h"
#include "../pdf/pdf_base14.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEXT_MAX_DEPTH 8     // Form XObject recursion cap
#define TEXT_OP_STACK 16     // operand stack (operators take at most 6)
#define TEXT_GS_STACK 32     // q/Q nesting kept
#define TOU_MAX_UNITS 8      // UTF-16 units per ToUnicode target (ligatures)

// --- Matrices (PDF row-vector convention: [a b 0; c d 0; e f 1]) ---

typedef struct { double a, b, c, d, e, f; } TxMat;

static const TxMat TX_IDENTITY = {1, 0, 0, 1, 0, 0};

// m applied first, then n
static TxMat tx_mul(TxMat m, TxMat n) {
    TxMat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

static TxMat tx_translate(double tx, double ty) {
    TxMat r = TX_IDENTITY;
    r.e = tx;
    r.f = ty;
    return r;
}

// --- Character helpers (content-stream lexing) ---

static bool tx_is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f';
}

static bool tx_is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

// --- Built-in encodings (byte -> Unicode code point, 0 = unmapped) ---

static uint16_t g_win[256], g_mac[256], g_std[256];
static bool g_tables_ready = false;

static void tx_init_tables(void) {
    if (g_tables_ready) return;
    memset(g_win, 0, sizeof(g_win));
    memset(g_mac, 0, sizeof(g_mac));
    memset(g_std, 0, sizeof(g_std));
    for (int i = 0x20; i <= 0x7E; i++) {
        g_win[i] = (uint16_t)i;
        g_mac[i] = (uint16_t)i;
        g_std[i] = (uint16_t)i;
    }
    // WinAnsiEncoding == cp1252: Latin-1 upper half + the 0x80-0x9F specials.
    for (int i = 0xA0; i <= 0xFF; i++) g_win[i] = (uint16_t)i;
    static const uint16_t win_high[32] = {
        0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
        0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178
    };
    for (int i = 0; i < 32; i++) g_win[0x80 + i] = win_high[i];
    static const uint16_t mac_high[128] = {
        0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
        0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
        0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
        0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
        0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
        0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
        0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
        0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
        0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
        0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
        0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
        0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
        0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
        0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
        0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
        0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
    };
    for (int i = 0; i < 128; i++) g_mac[0x80 + i] = mac_high[i];
    // Adobe StandardEncoding: curly quotes at 0x27/0x60, own upper half.
    g_std[0x27] = 0x2019;
    g_std[0x60] = 0x2018;
    static const struct { uint8_t code; uint16_t cp; } std_high[] = {
        {0xA1, 0x00A1}, {0xA2, 0x00A2}, {0xA3, 0x00A3}, {0xA4, 0x2044},
        {0xA5, 0x00A5}, {0xA6, 0x0192}, {0xA7, 0x00A7}, {0xA8, 0x00A4},
        {0xA9, 0x0027}, {0xAA, 0x201C}, {0xAB, 0x00AB}, {0xAC, 0x2039},
        {0xAD, 0x203A}, {0xAE, 0xFB01}, {0xAF, 0xFB02}, {0xB1, 0x2013},
        {0xB2, 0x2020}, {0xB3, 0x2021}, {0xB4, 0x00B7}, {0xB6, 0x00B6},
        {0xB7, 0x2022}, {0xB8, 0x201A}, {0xB9, 0x201E}, {0xBA, 0x201D},
        {0xBB, 0x00BB}, {0xBC, 0x2026}, {0xBD, 0x2030}, {0xBF, 0x00BF},
        {0xC1, 0x0060}, {0xC2, 0x00B4}, {0xC3, 0x02C6}, {0xC4, 0x02DC},
        {0xC5, 0x00AF}, {0xC6, 0x02D8}, {0xC7, 0x02D9}, {0xC8, 0x00A8},
        {0xCA, 0x02DA}, {0xCB, 0x00B8}, {0xCD, 0x02DD}, {0xCE, 0x02DB},
        {0xCF, 0x02C7}, {0xD0, 0x2014}, {0xE1, 0x00C6}, {0xE3, 0x00AA},
        {0xE8, 0x0141}, {0xE9, 0x00D8}, {0xEA, 0x0152}, {0xEB, 0x00BA},
        {0xF1, 0x00E6}, {0xF5, 0x0131}, {0xF8, 0x0142}, {0xF9, 0x00F8},
        {0xFA, 0x0153}, {0xFB, 0x00DF}
    };
    for (size_t i = 0; i < sizeof(std_high) / sizeof(std_high[0]); i++)
        g_std[std_high[i].code] = std_high[i].cp;
    g_tables_ready = true;
}

// --- AGL subset (glyph name -> Unicode) for /Differences ---

typedef struct { const char *name; uint16_t cp; } AglEntry;

static const AglEntry g_agl[] = {
    {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022},
    {"numbersign", 0x0023}, {"dollar", 0x0024}, {"percent", 0x0025},
    {"ampersand", 0x0026}, {"quotesingle", 0x0027}, {"parenleft", 0x0028},
    {"parenright", 0x0029}, {"asterisk", 0x002A}, {"plus", 0x002B},
    {"comma", 0x002C}, {"hyphen", 0x002D}, {"period", 0x002E},
    {"slash", 0x002F}, {"zero", 0x0030}, {"one", 0x0031}, {"two", 0x0032},
    {"three", 0x0033}, {"four", 0x0034}, {"five", 0x0035}, {"six", 0x0036},
    {"seven", 0x0037}, {"eight", 0x0038}, {"nine", 0x0039}, {"colon", 0x003A},
    {"semicolon", 0x003B}, {"less", 0x003C}, {"equal", 0x003D},
    {"greater", 0x003E}, {"question", 0x003F}, {"at", 0x0040},
    {"bracketleft", 0x005B}, {"backslash", 0x005C}, {"bracketright", 0x005D},
    {"asciicircum", 0x005E}, {"underscore", 0x005F}, {"grave", 0x0060},
    {"braceleft", 0x007B}, {"bar", 0x007C}, {"braceright", 0x007D},
    {"asciitilde", 0x007E},
    {"nbspace", 0x00A0}, {"exclamdown", 0x00A1}, {"cent", 0x00A2},
    {"sterling", 0x00A3}, {"currency", 0x00A4}, {"yen", 0x00A5},
    {"brokenbar", 0x00A6}, {"section", 0x00A7}, {"dieresis", 0x00A8},
    {"copyright", 0x00A9}, {"ordfeminine", 0x00AA}, {"guillemotleft", 0x00AB},
    {"logicalnot", 0x00AC}, {"sfthyphen", 0x00AD}, {"registered", 0x00AE},
    {"macron", 0x00AF}, {"degree", 0x00B0}, {"plusminus", 0x00B1},
    {"acute", 0x00B4}, {"mu", 0x00B5}, {"paragraph", 0x00B6},
    {"periodcentered", 0x00B7}, {"cedilla", 0x00B8}, {"ordmasculine", 0x00BA},
    {"guillemotright", 0x00BB}, {"onequarter", 0x00BC}, {"onehalf", 0x00BD},
    {"threequarters", 0x00BE}, {"questiondown", 0x00BF},
    {"Agrave", 0x00C0}, {"Aacute", 0x00C1}, {"Acircumflex", 0x00C2},
    {"Atilde", 0x00C3}, {"Adieresis", 0x00C4}, {"Aring", 0x00C5},
    {"AE", 0x00C6}, {"Ccedilla", 0x00C7}, {"Egrave", 0x00C8},
    {"Eacute", 0x00C9}, {"Ecircumflex", 0x00CA}, {"Edieresis", 0x00CB},
    {"Igrave", 0x00CC}, {"Iacute", 0x00CD}, {"Icircumflex", 0x00CE},
    {"Idieresis", 0x00CF}, {"Eth", 0x00D0}, {"Ntilde", 0x00D1},
    {"Ograve", 0x00D2}, {"Oacute", 0x00D3}, {"Ocircumflex", 0x00D4},
    {"Otilde", 0x00D5}, {"Odieresis", 0x00D6}, {"multiply", 0x00D7},
    {"Oslash", 0x00D8}, {"Ugrave", 0x00D9}, {"Uacute", 0x00DA},
    {"Ucircumflex", 0x00DB}, {"Udieresis", 0x00DC}, {"Yacute", 0x00DD},
    {"Thorn", 0x00DE}, {"germandbls", 0x00DF},
    {"agrave", 0x00E0}, {"aacute", 0x00E1}, {"acircumflex", 0x00E2},
    {"atilde", 0x00E3}, {"adieresis", 0x00E4}, {"aring", 0x00E5},
    {"ae", 0x00E6}, {"ccedilla", 0x00E7}, {"egrave", 0x00E8},
    {"eacute", 0x00E9}, {"ecircumflex", 0x00EA}, {"edieresis", 0x00EB},
    {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icircumflex", 0x00EE},
    {"idieresis", 0x00EF}, {"eth", 0x00F0}, {"ntilde", 0x00F1},
    {"ograve", 0x00F2}, {"oacute", 0x00F3}, {"ocircumflex", 0x00F4},
    {"otilde", 0x00F5}, {"odieresis", 0x00F6}, {"divide", 0x00F7},
    {"oslash", 0x00F8}, {"ugrave", 0x00F9}, {"uacute", 0x00FA},
    {"ucircumflex", 0x00FB}, {"udieresis", 0x00FC}, {"yacute", 0x00FD},
    {"thorn", 0x00FE}, {"ydieresis", 0x00FF},
    {"dotlessi", 0x0131}, {"Lslash", 0x0141}, {"lslash", 0x0142},
    {"OE", 0x0152}, {"oe", 0x0153}, {"Scaron", 0x0160}, {"scaron", 0x0161},
    {"Ydieresis", 0x0178}, {"Zcaron", 0x017D}, {"zcaron", 0x017E},
    {"florin", 0x0192},
    {"circumflex", 0x02C6}, {"caron", 0x02C7}, {"breve", 0x02D8},
    {"dotaccent", 0x02D9}, {"ring", 0x02DA}, {"ogonek", 0x02DB},
    {"tilde", 0x02DC}, {"hungarumlaut", 0x02DD},
    {"endash", 0x2013}, {"emdash", 0x2014}, {"quoteleft", 0x2018},
    {"quoteright", 0x2019}, {"quotesinglbase", 0x201A},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
    {"quotedblbase", 0x201E}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"perthousand", 0x2030},
    {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A},
    {"fraction", 0x2044}, {"Euro", 0x20AC}, {"trademark", 0x2122},
    {"minus", 0x2212}, {"fi", 0xFB01}, {"fl", 0xFB02},
};

static int tx_hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t tx_agl_lookup(const char *name) {
    size_t len = strlen(name);
    if (len == 1) {
        uint8_t ch = (uint8_t)name[0];
        return (ch >= 0x20 && ch <= 0x7E) ? ch : 0;
    }
    // uniXXXX and uXXXX/uXXXXX/uXXXXXX numeric forms
    if (len == 7 && memcmp(name, "uni", 3) == 0) {
        uint32_t v = 0;
        for (size_t i = 3; i < 7; i++) {
            int h = tx_hex_val((uint8_t)name[i]);
            if (h < 0) { v = 0; break; }
            v = (v << 4) | (uint32_t)h;
        }
        if (v) return v;
    }
    if (name[0] == 'u' && len >= 5 && len <= 7) {
        uint32_t v = 0;
        bool ok = true;
        for (size_t i = 1; i < len; i++) {
            int h = tx_hex_val((uint8_t)name[i]);
            if (h < 0) { ok = false; break; }
            v = (v << 4) | (uint32_t)h;
        }
        if (ok && v > 0 && v <= 0x10FFFF) return v;
    }
    for (size_t i = 0; i < sizeof(g_agl) / sizeof(g_agl[0]); i++) {
        if (strcmp(g_agl[i].name, name) == 0) return g_agl[i].cp;
    }
    return 0;
}

// --- Font model ---

typedef struct {
    uint32_t lo, hi;
    uint16_t units[TOU_MAX_UNITS]; // UTF-16 units for lo; last unit += code-lo
    uint8_t nunits;
} ToUniEntry;

typedef struct {
    uint32_t lo, hi;
    double w;        // used when per == NULL
    double *per;     // hi-lo+1 widths, scratch-arena
} CidWidth;

typedef struct TextFont {
    TspdfObj *dict;
    bool is_cid;                // Type0: 2-byte codes
    bool has_tou;
    ToUniEntry *tou;            // sorted by lo
    size_t tou_count;
    uint16_t simple_map[256];   // byte -> code point (0 = unmapped)
    double widths[256];         // simple-font advances (1/1000 text space)
    CidWidth *cw;               // sorted by lo
    size_t cw_count;
    double dw;                  // CID default width
    double space_w;             // space glyph width (1/1000), 0 = unknown
    struct TextFont *next;
} TextFont;

// --- Extraction context / graphics state ---

typedef struct {
    TspdfReader *doc;
    TspdfParser rp;          // object resolution (doc arena, persists in cache)
    TspdfArena scratch;      // per-extraction transient allocations
    TspdfBuffer out;
    TextFont *fonts;
    TextFont *fallback;
    size_t glyphs, replacements;
    bool have_last;
    double last_x, last_y, last_em;
    const TspdfObj *form_stack[TEXT_MAX_DEPTH]; // cycle guard, indexed by depth
} TextCtx;

typedef struct {
    TxMat ctm;
    TspdfObj *resources;
    TxMat tm, tlm;
    TextFont *font;
    double size, tc, tw, tl;
} TextGS;

static TspdfObj *tx_resolve(TextCtx *ctx, TspdfObj *o) {
    if (!o || o->type != TSPDF_OBJ_REF) return o;
    return tspdf_xref_resolve(&ctx->doc->xref, &ctx->rp, o->ref.num,
                              ctx->doc->obj_cache, ctx->doc->crypt);
}

static double tx_obj_num(const TspdfObj *o) {
    if (!o) return 0;
    if (o->type == TSPDF_OBJ_INT) return (double)o->integer;
    if (o->type == TSPDF_OBJ_REAL) return o->real;
    return 0;
}

// --- Stream loading (decrypt + filter chain, malloc'd result) ---

static uint8_t *tx_load_stream(TextCtx *ctx, TspdfObj *stream_obj,
                               uint32_t obj_num, size_t *out_len) {
    if (!stream_obj || stream_obj->type != TSPDF_OBJ_STREAM) return NULL;
    TspdfReader *doc = ctx->doc;
    const uint8_t *raw = NULL;
    size_t raw_len = 0;
    uint8_t *decrypted = NULL;

    if (stream_obj->stream.data && stream_obj->stream.self_contained) {
        // Overlay/merge-created stream: bytes already plaintext.
        raw = stream_obj->stream.data;
        raw_len = stream_obj->stream.len;
    } else {
        if (!doc->data || stream_obj->stream.raw_offset > doc->data_len ||
            stream_obj->stream.raw_len > doc->data_len - stream_obj->stream.raw_offset)
            return NULL;
        raw = doc->data + stream_obj->stream.raw_offset;
        raw_len = stream_obj->stream.raw_len;
        if (doc->crypt && obj_num > 0 && raw_len > 0) {
            size_t dec_len = raw_len;
            uint16_t gen = obj_num < doc->xref.count ? doc->xref.entries[obj_num].gen : 0;
            decrypted = tspdf_crypt_decrypt_stream(doc->crypt, obj_num, gen,
                                                   raw, raw_len, &dec_len);
            if (!decrypted) return NULL;
            raw = decrypted;
            raw_len = dec_len;
        }
    }

    uint8_t *decoded = NULL;
    size_t decoded_len = 0;
    TspdfError derr = tspdf_stream_decode(stream_obj->stream.dict, raw, raw_len,
                                          &decoded, &decoded_len);
    free(decrypted);
    if (derr != TSPDF_OK) return NULL;
    *out_len = decoded_len;
    return decoded;
}

// Concatenate a page's /Contents streams ('\n' between: stream boundaries are
// token boundaries per ISO 32000). Returns a malloc'd buffer via TspdfBuffer.
static void tx_load_page_content(TextCtx *ctx, TspdfObj *page_dict, TspdfBuffer *buf) {
    TspdfObj *contents = tspdf_dict_get(page_dict, "Contents");
    if (!contents) return;

    TspdfObj *items[1] = {NULL};
    TspdfObj *arr = NULL;
    size_t count = 0;
    if (contents->type == TSPDF_OBJ_ARRAY) {
        arr = contents;
    } else {
        TspdfObj *resolved = tx_resolve(ctx, contents);
        if (resolved && resolved->type == TSPDF_OBJ_ARRAY) {
            arr = resolved;
        } else {
            items[0] = contents;
            count = 1;
        }
    }
    size_t n = arr ? arr->array.count : count;
    for (size_t i = 0; i < n; i++) {
        TspdfObj *item = arr ? &arr->array.items[i] : items[i];
        uint32_t num = item->type == TSPDF_OBJ_REF ? item->ref.num : 0;
        TspdfObj *s = tx_resolve(ctx, item);
        if (!s || s->type != TSPDF_OBJ_STREAM) continue;
        size_t slen = 0;
        uint8_t *bytes = tx_load_stream(ctx, s, num, &slen);
        if (!bytes) continue;
        tspdf_buffer_append(buf, bytes, slen);
        tspdf_buffer_append_byte(buf, '\n');
        free(bytes);
    }
}

// Page /Resources with Pages-tree inheritance via the /Parent chain.
static TspdfObj *tx_page_resources(TextCtx *ctx, TspdfObj *page_dict) {
    TspdfObj *cur = page_dict;
    for (size_t depth = 0; cur && cur->type == TSPDF_OBJ_DICT && depth < 64; depth++) {
        TspdfObj *res = tx_resolve(ctx, tspdf_dict_get(cur, "Resources"));
        if (res && res->type == TSPDF_OBJ_DICT) return res;
        cur = tx_resolve(ctx, tspdf_dict_get(cur, "Parent"));
    }
    return NULL;
}

// --- ToUnicode CMap parsing ---

typedef struct { ToUniEntry *v; size_t n, cap; } ToUniList;

static void tou_push(TextCtx *ctx, ToUniList *l, ToUniEntry e) {
    if (l->n >= l->cap) {
        size_t new_cap = l->cap == 0 ? 32 : l->cap * 2;
        ToUniEntry *nv = (ToUniEntry *)tspdf_arena_alloc(&ctx->scratch,
                                                         new_cap * sizeof(ToUniEntry));
        if (!nv) return;
        if (l->v && l->n) memcpy(nv, l->v, l->n * sizeof(ToUniEntry));
        l->v = nv;
        l->cap = new_cap;
    }
    l->v[l->n++] = e;
}

static bool tx_str_code(const TspdfObj *o, uint32_t *out) {
    if (!o || o->type != TSPDF_OBJ_STRING || o->string.len == 0 || o->string.len > 4)
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < o->string.len; i++) v = (v << 8) | o->string.data[i];
    *out = v;
    return true;
}

static bool tx_str_units(const TspdfObj *o, uint16_t *units, uint8_t *n) {
    if (!o || o->type != TSPDF_OBJ_STRING || o->string.len < 2) return false;
    size_t nu = o->string.len / 2;
    if (nu > TOU_MAX_UNITS) nu = TOU_MAX_UNITS;
    for (size_t i = 0; i < nu; i++)
        units[i] = (uint16_t)((o->string.data[2 * i] << 8) | o->string.data[2 * i + 1]);
    *n = (uint8_t)nu;
    return true;
}

static int tou_cmp(const void *a, const void *b) {
    const ToUniEntry *ea = (const ToUniEntry *)a, *eb = (const ToUniEntry *)b;
    if (ea->lo < eb->lo) return -1;
    if (ea->lo > eb->lo) return 1;
    return 0;
}

// True if the parser is positioned at a string operand ('<' hex or '(' literal).
static bool tx_at_string(TspdfParser *p) {
    if (p->pos >= p->len) return false;
    uint8_t c = p->data[p->pos];
    if (c == '(') return true;
    if (c == '<') return !(p->pos + 1 < p->len && p->data[p->pos + 1] == '<');
    return false;
}

static void tx_parse_tounicode(TextCtx *ctx, TextFont *f,
                               const uint8_t *data, size_t len) {
    TspdfParser p;
    tspdf_parser_init(&p, data, len, &ctx->scratch);
    ToUniList list = {0};

    while (1) {
        tspdf_skip_whitespace(&p);
        if (p.pos >= p.len) break;
        uint8_t c = p.data[p.pos];
        if (c == '(' || c == '<' || c == '[' || c == '/' || c == '+' ||
            c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            // Operand outside a bf block (codespace ranges etc.): parse + drop.
            p.error = TSPDF_OK;
            size_t before = p.pos;
            if (!tspdf_parse_obj(&p)) { p.error = TSPDF_OK; p.pos = before + 1; }
            continue;
        }
        if (tx_is_delim(c)) { p.pos++; continue; }
        char kw[24];
        size_t kl = 0;
        while (p.pos < p.len && !tx_is_ws(p.data[p.pos]) && !tx_is_delim(p.data[p.pos])) {
            if (kl + 1 < sizeof(kw)) kw[kl++] = (char)p.data[p.pos];
            p.pos++;
        }
        kw[kl] = '\0';
        if (kl == 0) { p.pos++; continue; }

        if (strcmp(kw, "beginbfchar") == 0) {
            while (1) {
                tspdf_skip_whitespace(&p);
                if (!tx_at_string(&p)) break; // endbfchar or malformed
                p.error = TSPDF_OK;
                TspdfObj *src = tspdf_parse_obj(&p);
                tspdf_skip_whitespace(&p);
                TspdfObj *dst = tx_at_string(&p) ? tspdf_parse_obj(&p) : NULL;
                uint32_t code;
                ToUniEntry e;
                memset(&e, 0, sizeof(e));
                if (!src || !dst || !tx_str_code(src, &code) ||
                    !tx_str_units(dst, e.units, &e.nunits))
                    break;
                e.lo = e.hi = code;
                tou_push(ctx, &list, e);
            }
        } else if (strcmp(kw, "beginbfrange") == 0) {
            while (1) {
                tspdf_skip_whitespace(&p);
                if (!tx_at_string(&p)) break; // endbfrange or malformed
                p.error = TSPDF_OK;
                TspdfObj *lo_o = tspdf_parse_obj(&p);
                tspdf_skip_whitespace(&p);
                TspdfObj *hi_o = tx_at_string(&p) ? tspdf_parse_obj(&p) : NULL;
                uint32_t lo, hi;
                if (!lo_o || !hi_o || !tx_str_code(lo_o, &lo) || !tx_str_code(hi_o, &hi))
                    break;
                if (hi < lo) { uint32_t t = lo; lo = hi; hi = t; }
                if (hi - lo > 0xFFFF) hi = lo + 0xFFFF; // hostile range cap
                tspdf_skip_whitespace(&p);
                if (p.pos < p.len && p.data[p.pos] == '[') {
                    TspdfObj *arr = tspdf_parse_obj(&p);
                    if (!arr || arr->type != TSPDF_OBJ_ARRAY) break;
                    for (size_t i = 0; i < arr->array.count && lo + i <= hi; i++) {
                        ToUniEntry e;
                        memset(&e, 0, sizeof(e));
                        if (!tx_str_units(&arr->array.items[i], e.units, &e.nunits))
                            continue;
                        e.lo = e.hi = lo + (uint32_t)i;
                        tou_push(ctx, &list, e);
                    }
                } else if (tx_at_string(&p)) {
                    TspdfObj *dst = tspdf_parse_obj(&p);
                    ToUniEntry e;
                    memset(&e, 0, sizeof(e));
                    if (!dst || !tx_str_units(dst, e.units, &e.nunits)) break;
                    e.lo = lo;
                    e.hi = hi;
                    tou_push(ctx, &list, e);
                } else {
                    break;
                }
            }
        } else if (strcmp(kw, "endcmap") == 0) {
            break;
        }
    }

    if (list.n > 0) {
        qsort(list.v, list.n, sizeof(ToUniEntry), tou_cmp);
        f->tou = list.v;
        f->tou_count = list.n;
        f->has_tou = true;
    }
}

static bool tou_lookup(const TextFont *f, uint32_t code,
                       uint16_t *units, uint8_t *n) {
    if (!f->tou_count) return false;
    // Binary search for the last entry with lo <= code, then walk back a few
    // entries to tolerate overlapping ranges.
    size_t lo = 0, hi = f->tou_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->tou[mid].lo <= code) lo = mid + 1;
        else hi = mid;
    }
    size_t idx = lo;
    for (int back = 0; idx > 0 && back < 4; back++) {
        const ToUniEntry *e = &f->tou[--idx];
        if (e->lo > code) continue;
        if (code > e->hi) continue;
        if (e->nunits == 0) return false;
        memcpy(units, e->units, e->nunits * sizeof(uint16_t));
        units[e->nunits - 1] = (uint16_t)(units[e->nunits - 1] + (code - e->lo));
        *n = e->nunits;
        return true;
    }
    return false;
}

// --- Widths ---

static void tx_simple_widths(TextCtx *ctx, TextFont *f) {
    const TspdfBase14Metrics *b14 = NULL;
    TspdfObj *base = tx_resolve(ctx, tspdf_dict_get(f->dict, "BaseFont"));
    if (base && base->type == TSPDF_OBJ_NAME) {
        const char *nm = (const char *)base->string.data;
        const char *plus = strchr(nm, '+'); // subset tag: "ABCDEF+Name"
        b14 = tspdf_base14_get(plus ? plus + 1 : nm);
    }

    for (int i = 0; i < 256; i++) f->widths[i] = 0;
    bool have_widths = false;
    TspdfObj *fc = tx_resolve(ctx, tspdf_dict_get(f->dict, "FirstChar"));
    TspdfObj *ws = tx_resolve(ctx, tspdf_dict_get(f->dict, "Widths"));
    if (fc && fc->type == TSPDF_OBJ_INT && ws && ws->type == TSPDF_OBJ_ARRAY) {
        for (size_t i = 0; i < ws->array.count; i++) {
            int64_t code = fc->integer + (int64_t)i;
            if (code < 0 || code > 255) continue;
            TspdfObj *wv = tx_resolve(ctx, &ws->array.items[i]);
            double w = tx_obj_num(wv);
            if (w > 0) { f->widths[code] = w; have_widths = true; }
        }
    }

    double missing = 0;
    TspdfObj *fd = tx_resolve(ctx, tspdf_dict_get(f->dict, "FontDescriptor"));
    if (fd && fd->type == TSPDF_OBJ_DICT) {
        missing = tx_obj_num(tx_resolve(ctx, tspdf_dict_get(fd, "MissingWidth")));
    }
    for (int i = 0; i < 256; i++) {
        if (f->widths[i] > 0) continue;
        if (have_widths && missing > 0) f->widths[i] = missing;
        else if (b14 && b14->widths[i] > 0) f->widths[i] = b14->widths[i];
        else f->widths[i] = 500;
    }
    f->space_w = f->widths[32];
}

static int cid_w_cmp(const void *a, const void *b) {
    const CidWidth *wa = (const CidWidth *)a, *wb = (const CidWidth *)b;
    if (wa->lo < wb->lo) return -1;
    if (wa->lo > wb->lo) return 1;
    return 0;
}

static void tx_cid_widths(TextCtx *ctx, TextFont *f, TspdfObj *desc) {
    f->dw = 1000;
    double dw = tx_obj_num(tx_resolve(ctx, tspdf_dict_get(desc, "DW")));
    if (dw > 0) f->dw = dw;

    TspdfObj *w = tx_resolve(ctx, tspdf_dict_get(desc, "W"));
    if (!w || w->type != TSPDF_OBJ_ARRAY) return;

    size_t cap = 0, n = 0;
    CidWidth *v = NULL;
    size_t i = 0;
    while (i < w->array.count) {
        TspdfObj *c1 = tx_resolve(ctx, &w->array.items[i]);
        if (!c1 || c1->type != TSPDF_OBJ_INT || c1->integer < 0) break;
        if (i + 1 >= w->array.count) break;
        TspdfObj *second = tx_resolve(ctx, &w->array.items[i + 1]);
        CidWidth cw;
        memset(&cw, 0, sizeof(cw));
        if (second && second->type == TSPDF_OBJ_ARRAY) {
            // c [w1 w2 ...]
            size_t cnt = second->array.count;
            if (cnt == 0 || cnt > 0x10000) { i += 2; continue; }
            double *per = (double *)tspdf_arena_alloc(&ctx->scratch, cnt * sizeof(double));
            if (!per) break;
            for (size_t j = 0; j < cnt; j++)
                per[j] = tx_obj_num(tx_resolve(ctx, &second->array.items[j]));
            cw.lo = (uint32_t)c1->integer;
            cw.hi = cw.lo + (uint32_t)cnt - 1;
            cw.per = per;
            i += 2;
        } else if (second && second->type == TSPDF_OBJ_INT && i + 2 < w->array.count) {
            // c1 c2 w
            TspdfObj *wv = tx_resolve(ctx, &w->array.items[i + 2]);
            if (second->integer < c1->integer) { i += 3; continue; }
            cw.lo = (uint32_t)c1->integer;
            cw.hi = (uint32_t)second->integer;
            cw.w = tx_obj_num(wv);
            i += 3;
        } else {
            break;
        }
        if (n >= cap) {
            size_t new_cap = cap == 0 ? 16 : cap * 2;
            CidWidth *nv = (CidWidth *)tspdf_arena_alloc(&ctx->scratch,
                                                         new_cap * sizeof(CidWidth));
            if (!nv) break;
            if (v && n) memcpy(nv, v, n * sizeof(CidWidth));
            v = nv;
            cap = new_cap;
        }
        v[n++] = cw;
    }
    if (n > 0) {
        qsort(v, n, sizeof(CidWidth), cid_w_cmp);
        f->cw = v;
        f->cw_count = n;
    }
}

static double tx_cid_width(const TextFont *f, uint32_t code) {
    size_t lo = 0, hi = f->cw_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->cw[mid].lo <= code) lo = mid + 1;
        else hi = mid;
    }
    size_t idx = lo;
    for (int back = 0; idx > 0 && back < 4; back++) {
        const CidWidth *e = &f->cw[--idx];
        if (e->lo > code || code > e->hi) continue;
        double w = e->per ? e->per[code - e->lo] : e->w;
        return w > 0 ? w : f->dw;
    }
    return f->dw;
}

// --- Simple-font encoding (/Encoding name/dict + /Differences) ---

static const uint16_t *tx_table_for_name(const char *name) {
    if (strcmp(name, "WinAnsiEncoding") == 0) return g_win;
    if (strcmp(name, "MacRomanEncoding") == 0) return g_mac;
    return g_std; // StandardEncoding and anything unknown
}

static void tx_simple_encoding(TextCtx *ctx, TextFont *f) {
    tx_init_tables();
    const uint16_t *base = g_std;
    TspdfObj *diffs = NULL;
    TspdfObj *enc = tx_resolve(ctx, tspdf_dict_get(f->dict, "Encoding"));
    if (enc && enc->type == TSPDF_OBJ_NAME) {
        base = tx_table_for_name((const char *)enc->string.data);
    } else if (enc && enc->type == TSPDF_OBJ_DICT) {
        TspdfObj *be = tx_resolve(ctx, tspdf_dict_get(enc, "BaseEncoding"));
        if (be && be->type == TSPDF_OBJ_NAME)
            base = tx_table_for_name((const char *)be->string.data);
        diffs = tx_resolve(ctx, tspdf_dict_get(enc, "Differences"));
    }
    memcpy(f->simple_map, base, sizeof(f->simple_map));
    if (diffs && diffs->type == TSPDF_OBJ_ARRAY) {
        int64_t code = -1;
        for (size_t i = 0; i < diffs->array.count; i++) {
            TspdfObj *item = tx_resolve(ctx, &diffs->array.items[i]);
            if (!item) continue;
            if (item->type == TSPDF_OBJ_INT) {
                code = item->integer;
            } else if (item->type == TSPDF_OBJ_NAME) {
                if (code >= 0 && code <= 255) {
                    uint32_t cp = tx_agl_lookup((const char *)item->string.data);
                    f->simple_map[code] = cp <= 0xFFFF ? (uint16_t)cp : 0;
                }
                code++;
            }
        }
    }
}

// --- Font cache ---

static TextFont *tx_fallback_font(TextCtx *ctx) {
    if (ctx->fallback) return ctx->fallback;
    TextFont *f = (TextFont *)tspdf_arena_alloc_zero(&ctx->scratch, sizeof(TextFont));
    if (!f) return NULL;
    tx_init_tables();
    memcpy(f->simple_map, g_std, sizeof(f->simple_map));
    for (int i = 0; i < 256; i++) f->widths[i] = 500;
    f->space_w = 250;
    f->dw = 1000;
    ctx->fallback = f;
    return f;
}

static TextFont *tx_get_font(TextCtx *ctx, TspdfObj *resources,
                             const TspdfObj *name_obj) {
    if (!resources || !name_obj || name_obj->type != TSPDF_OBJ_NAME) return NULL;
    TspdfObj *fonts = tx_resolve(ctx, tspdf_dict_get(resources, "Font"));
    if (!fonts || fonts->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *fd = tx_resolve(ctx, tspdf_dict_get(fonts, (const char *)name_obj->string.data));
    if (!fd || fd->type != TSPDF_OBJ_DICT) return NULL;

    for (TextFont *f = ctx->fonts; f; f = f->next)
        if (f->dict == fd) return f;

    TextFont *f = (TextFont *)tspdf_arena_alloc_zero(&ctx->scratch, sizeof(TextFont));
    if (!f) return NULL;
    f->dict = fd;
    TspdfObj *sub = tx_resolve(ctx, tspdf_dict_get(fd, "Subtype"));
    f->is_cid = sub && sub->type == TSPDF_OBJ_NAME &&
                strcmp((const char *)sub->string.data, "Type0") == 0;

    TspdfObj *touref = tspdf_dict_get(fd, "ToUnicode");
    uint32_t tou_num = touref && touref->type == TSPDF_OBJ_REF ? touref->ref.num : 0;
    TspdfObj *tou = tx_resolve(ctx, touref);
    if (tou && tou->type == TSPDF_OBJ_STREAM) {
        size_t clen = 0;
        uint8_t *cmap = tx_load_stream(ctx, tou, tou_num, &clen);
        if (cmap) {
            tx_parse_tounicode(ctx, f, cmap, clen);
            free(cmap);
        }
    }

    if (f->is_cid) {
        f->dw = 1000;
        TspdfObj *dfs = tx_resolve(ctx, tspdf_dict_get(fd, "DescendantFonts"));
        if (dfs && dfs->type == TSPDF_OBJ_ARRAY && dfs->array.count > 0) {
            TspdfObj *desc = tx_resolve(ctx, &dfs->array.items[0]);
            if (desc && desc->type == TSPDF_OBJ_DICT) tx_cid_widths(ctx, f, desc);
        }
    } else {
        tx_simple_encoding(ctx, f);
        tx_simple_widths(ctx, f);
    }

    f->next = ctx->fonts;
    ctx->fonts = f;
    return f;
}

// --- Text emission + layout heuristics ---

static void tx_emit_cp(TextCtx *ctx, uint32_t cp) {
    char buf[4];
    size_t n = tspdf_utf8_encode(cp, buf);
    tspdf_buffer_append(&ctx->out, buf, n);
}

static void tx_emit_units(TextCtx *ctx, const uint16_t *units, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        tx_emit_cp(ctx, cp);
    }
}

static void tx_newline(TextCtx *ctx) {
    if (ctx->out.len > 0 && ctx->out.data[ctx->out.len - 1] != '\n')
        tspdf_buffer_append_byte(&ctx->out, '\n');
}

static void tx_space(TextCtx *ctx) {
    if (ctx->out.len > 0) {
        uint8_t last = ctx->out.data[ctx->out.len - 1];
        if (last != ' ' && last != '\n')
            tspdf_buffer_append_byte(&ctx->out, ' ');
    }
}

// Device-space text origin, em height, and x scale (per text-space unit).
static void tx_device_state(const TextGS *gs, double *x, double *y,
                            double *em, double *sx) {
    TxMat trm = tx_mul(gs->tm, gs->ctm);
    *x = trm.e;
    *y = trm.f;
    double size = gs->size != 0 ? fabs(gs->size) : 1.0;
    double emv = size * sqrt(trm.c * trm.c + trm.d * trm.d);
    if (emv <= 0 || !isfinite(emv)) emv = 1.0;
    *em = emv;
    double sxv = sqrt(trm.a * trm.a + trm.b * trm.b);
    if (sxv <= 0 || !isfinite(sxv)) sxv = 1.0;
    *sx = sxv;
}

// Compare the run start against the previous run end: y-jump beyond 0.3 em
// emits a newline, an x-gap wider than ~half a space emits a space.
static void tx_pre_run(TextCtx *ctx, const TextGS *gs) {
    double x, y, em, sx;
    tx_device_state(gs, &x, &y, &em, &sx);
    if (ctx->have_last && ctx->out.len > 0) {
        double ref_em = em > ctx->last_em ? em : ctx->last_em;
        if (ref_em <= 0) ref_em = 1.0;
        double dy = fabs(y - ctx->last_y);
        double dx = x - ctx->last_x;
        if (dy > 0.3 * ref_em) {
            tx_newline(ctx);
        } else {
            double size = gs->size != 0 ? fabs(gs->size) : 1.0;
            double space_w = gs->font && gs->font->space_w > 0 ? gs->font->space_w : 250;
            double thr = 0.5 * (space_w / 1000.0) * size * sx;
            double min_thr = 0.15 * ref_em;
            if (thr < min_thr) thr = min_thr;
            if (dx > thr || dx < -ref_em) tx_space(ctx);
        }
    }
}

static void tx_post_run(TextCtx *ctx, const TextGS *gs) {
    double x, y, em, sx;
    tx_device_state(gs, &x, &y, &em, &sx);
    ctx->last_x = x;
    ctx->last_y = y;
    ctx->last_em = em;
    ctx->have_last = true;
}

// Decode and emit one shown string, advancing the text matrix by the sum of
// glyph advances (widths in 1/1000 text space + Tc, + Tw at code 32).
static void tx_show_string(TextCtx *ctx, TextGS *gs, const TspdfObj *str) {
    if (!str || str->type != TSPDF_OBJ_STRING || str->string.len == 0) return;
    TextFont *f = gs->font ? gs->font : tx_fallback_font(ctx);
    if (!f) return;
    const uint8_t *b = str->string.data;
    size_t n = str->string.len;
    double adv = 0;

    if (f->is_cid) {
        for (size_t i = 0; i < n; i += 2) {
            uint32_t code = i + 1 < n ? ((uint32_t)b[i] << 8) | b[i + 1] : b[i];
            ctx->glyphs++;
            uint16_t units[TOU_MAX_UNITS];
            uint8_t nu = 0;
            if (f->has_tou && tou_lookup(f, code, units, &nu)) {
                tx_emit_units(ctx, units, nu);
            } else {
                tx_emit_cp(ctx, 0xFFFD);
                ctx->replacements++;
            }
            adv += tx_cid_width(f, code) / 1000.0 * gs->size + gs->tc;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            uint32_t code = b[i];
            ctx->glyphs++;
            uint16_t units[TOU_MAX_UNITS];
            uint8_t nu = 0;
            if (f->has_tou && tou_lookup(f, code, units, &nu)) {
                tx_emit_units(ctx, units, nu);
            } else if (f->simple_map[code]) {
                tx_emit_cp(ctx, f->simple_map[code]);
            } else {
                tx_emit_cp(ctx, 0xFFFD);
                ctx->replacements++;
            }
            adv += f->widths[code] / 1000.0 * gs->size + gs->tc +
                   (code == 32 ? gs->tw : 0);
        }
    }
    gs->tm = tx_mul(tx_translate(adv, 0), gs->tm);
}

// --- Interpreter ---

static void tx_interpret(TextCtx *ctx, const uint8_t *data, size_t len,
                         TextGS *gs, int depth);

static void tx_do_xobject(TextCtx *ctx, const TextGS *gs,
                          const TspdfObj *name, int depth) {
    if (depth + 1 >= TEXT_MAX_DEPTH) return;
    if (!gs->resources || !name || name->type != TSPDF_OBJ_NAME) return;
    TspdfObj *xobjs = tx_resolve(ctx, tspdf_dict_get(gs->resources, "XObject"));
    if (!xobjs || xobjs->type != TSPDF_OBJ_DICT) return;
    TspdfObj *ref = tspdf_dict_get(xobjs, (const char *)name->string.data);
    uint32_t num = ref && ref->type == TSPDF_OBJ_REF ? ref->ref.num : 0;
    TspdfObj *xo = tx_resolve(ctx, ref);
    if (!xo || xo->type != TSPDF_OBJ_STREAM) return;
    TspdfObj *sub = tx_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Subtype"));
    if (!sub || sub->type != TSPDF_OBJ_NAME ||
        strcmp((const char *)sub->string.data, "Form") != 0)
        return;
    for (int i = 0; i <= depth; i++)
        if (ctx->form_stack[i] == xo) return; // cycle
    ctx->form_stack[depth + 1] = xo;

    size_t blen = 0;
    uint8_t *bytes = tx_load_stream(ctx, xo, num, &blen);
    if (!bytes) return;

    TextGS sub_gs = *gs;
    sub_gs.tm = sub_gs.tlm = TX_IDENTITY;
    TspdfObj *mtx = tx_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Matrix"));
    if (mtx && mtx->type == TSPDF_OBJ_ARRAY && mtx->array.count >= 6) {
        TxMat m;
        m.a = tx_obj_num(tx_resolve(ctx, &mtx->array.items[0]));
        m.b = tx_obj_num(tx_resolve(ctx, &mtx->array.items[1]));
        m.c = tx_obj_num(tx_resolve(ctx, &mtx->array.items[2]));
        m.d = tx_obj_num(tx_resolve(ctx, &mtx->array.items[3]));
        m.e = tx_obj_num(tx_resolve(ctx, &mtx->array.items[4]));
        m.f = tx_obj_num(tx_resolve(ctx, &mtx->array.items[5]));
        sub_gs.ctm = tx_mul(m, sub_gs.ctm);
    }
    TspdfObj *res = tx_resolve(ctx, tspdf_dict_get(xo->stream.dict, "Resources"));
    if (res && res->type == TSPDF_OBJ_DICT) sub_gs.resources = res;

    tx_interpret(ctx, bytes, blen, &sub_gs, depth + 1);
    free(bytes);
    ctx->form_stack[depth + 1] = NULL;
}

// Skip BI ... ID <binary> EI: the raw image bytes would derail the tokenizer.
static void tx_skip_inline_image(TspdfParser *p) {
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'I' && p->data[p->pos + 1] == 'D' &&
            (p->pos + 2 >= p->len || tx_is_ws(p->data[p->pos + 2]))) {
            p->pos += 2;
            break;
        }
        p->pos++;
    }
    if (p->pos < p->len && tx_is_ws(p->data[p->pos])) p->pos++;
    while (p->pos + 1 < p->len) {
        if (p->data[p->pos] == 'E' && p->data[p->pos + 1] == 'I' &&
            p->pos > 0 && tx_is_ws(p->data[p->pos - 1]) &&
            (p->pos + 2 >= p->len || tx_is_ws(p->data[p->pos + 2]) ||
             tx_is_delim(p->data[p->pos + 2]))) {
            p->pos += 2;
            return;
        }
        p->pos++;
    }
    p->pos = p->len;
}

static TspdfObj *stk_at(TspdfObj **stk, int ns, int from_top) {
    int i = ns - 1 - from_top;
    return i >= 0 ? stk[i] : NULL;
}

static double stk_num(TspdfObj **stk, int ns, int from_top) {
    return tx_obj_num(stk_at(stk, ns, from_top));
}

static void tx_op_td(TextGS *gs, double tx, double ty) {
    gs->tlm = tx_mul(tx_translate(tx, ty), gs->tlm);
    gs->tm = gs->tlm;
}

static void tx_interpret(TextCtx *ctx, const uint8_t *data, size_t len,
                         TextGS *gs, int depth) {
    TspdfParser p;
    tspdf_parser_init(&p, data, len, &ctx->scratch);
    TspdfObj *stk[TEXT_OP_STACK];
    int ns = 0;
    TextGS gstack[TEXT_GS_STACK];
    int ngs = 0;

    while (1) {
        tspdf_skip_whitespace(&p);
        if (p.pos >= p.len) break;
        uint8_t c = p.data[p.pos];

        if (c == '(' || c == '<' || c == '[' || c == '/' || c == '+' ||
            c == '-' || c == '.' || (c >= '0' && c <= '9')) {
            p.error = TSPDF_OK;
            size_t before = p.pos;
            TspdfObj *o = tspdf_parse_obj(&p);
            if (!o) {
                // Malformed operand: resync one byte further, drop the stack.
                p.error = TSPDF_OK;
                p.pos = before + 1;
                ns = 0;
                continue;
            }
            if (ns == TEXT_OP_STACK) {
                memmove(stk, stk + 1, sizeof(stk[0]) * (TEXT_OP_STACK - 1));
                ns--;
            }
            stk[ns++] = o;
            continue;
        }
        if (tx_is_delim(c)) { // stray ) > ] } { — resync
            p.pos++;
            continue;
        }

        char op[8];
        size_t ol = 0;
        bool overlong = false;
        while (p.pos < p.len && !tx_is_ws(p.data[p.pos]) && !tx_is_delim(p.data[p.pos]) &&
               p.data[p.pos] > ' ') {
            if (ol + 1 < sizeof(op)) op[ol++] = (char)p.data[p.pos];
            else overlong = true;
            p.pos++;
        }
        op[ol] = '\0';
        if (ol == 0) { p.pos++; continue; }
        if (overlong) { ns = 0; continue; }

        // "true"/"false"/"null" are operands, not operators.
        if (strcmp(op, "true") == 0 || strcmp(op, "false") == 0 ||
            strcmp(op, "null") == 0) {
            continue; // no text operator consumes them; keep stack as-is
        }

        if (strcmp(op, "BT") == 0) {
            gs->tm = gs->tlm = TX_IDENTITY;
        } else if (strcmp(op, "ET") == 0) {
            // nothing
        } else if (strcmp(op, "Tf") == 0) {
            gs->size = stk_num(stk, ns, 0);
            gs->font = tx_get_font(ctx, gs->resources, stk_at(stk, ns, 1));
        } else if (strcmp(op, "Td") == 0) {
            tx_op_td(gs, stk_num(stk, ns, 1), stk_num(stk, ns, 0));
        } else if (strcmp(op, "TD") == 0) {
            gs->tl = -stk_num(stk, ns, 0);
            tx_op_td(gs, stk_num(stk, ns, 1), stk_num(stk, ns, 0));
        } else if (strcmp(op, "Tm") == 0) {
            TxMat m;
            m.a = stk_num(stk, ns, 5);
            m.b = stk_num(stk, ns, 4);
            m.c = stk_num(stk, ns, 3);
            m.d = stk_num(stk, ns, 2);
            m.e = stk_num(stk, ns, 1);
            m.f = stk_num(stk, ns, 0);
            gs->tm = gs->tlm = m;
        } else if (strcmp(op, "T*") == 0) {
            tx_op_td(gs, 0, -gs->tl);
        } else if (strcmp(op, "TL") == 0) {
            gs->tl = stk_num(stk, ns, 0);
        } else if (strcmp(op, "Tc") == 0) {
            gs->tc = stk_num(stk, ns, 0);
        } else if (strcmp(op, "Tw") == 0) {
            gs->tw = stk_num(stk, ns, 0);
        } else if (strcmp(op, "Tz") == 0 || strcmp(op, "Ts") == 0 ||
                   strcmp(op, "Tr") == 0) {
            // horizontal scale / rise / render mode: explicitly ignored
        } else if (strcmp(op, "Tj") == 0) {
            tx_pre_run(ctx, gs);
            tx_show_string(ctx, gs, stk_at(stk, ns, 0));
            tx_post_run(ctx, gs);
        } else if (strcmp(op, "TJ") == 0) {
            TspdfObj *arr = stk_at(stk, ns, 0);
            if (arr && arr->type == TSPDF_OBJ_ARRAY) {
                tx_pre_run(ctx, gs);
                double thr = gs->font && gs->font->space_w > 0
                                 ? gs->font->space_w * 0.5 : 120.0;
                for (size_t i = 0; i < arr->array.count; i++) {
                    TspdfObj *el = &arr->array.items[i];
                    if (el->type == TSPDF_OBJ_STRING) {
                        tx_show_string(ctx, gs, el);
                    } else if (el->type == TSPDF_OBJ_INT ||
                               el->type == TSPDF_OBJ_REAL) {
                        double v = tx_obj_num(el);
                        if (-v > thr) tx_space(ctx);
                        gs->tm = tx_mul(tx_translate(-v / 1000.0 * gs->size, 0),
                                        gs->tm);
                    }
                }
                tx_post_run(ctx, gs);
            }
        } else if (strcmp(op, "'") == 0) {
            tx_op_td(gs, 0, -gs->tl);
            tx_pre_run(ctx, gs);
            tx_show_string(ctx, gs, stk_at(stk, ns, 0));
            tx_post_run(ctx, gs);
        } else if (strcmp(op, "\"") == 0) {
            gs->tw = stk_num(stk, ns, 2);
            gs->tc = stk_num(stk, ns, 1);
            tx_op_td(gs, 0, -gs->tl);
            tx_pre_run(ctx, gs);
            tx_show_string(ctx, gs, stk_at(stk, ns, 0));
            tx_post_run(ctx, gs);
        } else if (strcmp(op, "q") == 0) {
            if (ngs < TEXT_GS_STACK) gstack[ngs++] = *gs;
        } else if (strcmp(op, "Q") == 0) {
            if (ngs > 0) *gs = gstack[--ngs];
        } else if (strcmp(op, "cm") == 0) {
            TxMat m;
            m.a = stk_num(stk, ns, 5);
            m.b = stk_num(stk, ns, 4);
            m.c = stk_num(stk, ns, 3);
            m.d = stk_num(stk, ns, 2);
            m.e = stk_num(stk, ns, 1);
            m.f = stk_num(stk, ns, 0);
            gs->ctm = tx_mul(m, gs->ctm);
        } else if (strcmp(op, "Do") == 0) {
            tx_do_xobject(ctx, gs, stk_at(stk, ns, 0), depth);
        } else if (strcmp(op, "BI") == 0) {
            tx_skip_inline_image(&p);
        }
        // every other operator: skipped
        ns = 0;
    }
}

// --- Public API ---

const char *tspdf_reader_page_text_stats(TspdfReader *doc, size_t page_index,
                                         TspdfTextStats *stats, TspdfError *err) {
    if (stats) {
        stats->glyphs = 0;
        stats->replacements = 0;
    }
    if (!doc) {
        if (err) *err = TSPDF_ERR_PARSE;
        return NULL;
    }
    if (page_index >= doc->pages.count) {
        if (err) *err = TSPDF_ERR_PAGE_RANGE;
        return NULL;
    }

    TextCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.doc = doc;
    tspdf_parser_init(&ctx.rp, doc->data, doc->data_len, &doc->arena);
    ctx.scratch = tspdf_arena_create(65536);
    if (!ctx.scratch.first) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    ctx.out = tspdf_buffer_create(4096);

    TspdfReaderPage *page = &doc->pages.pages[page_index];
    TextGS gs;
    memset(&gs, 0, sizeof(gs));
    gs.ctm = TX_IDENTITY;
    gs.tm = gs.tlm = TX_IDENTITY;
    gs.resources = tx_page_resources(&ctx, page->page_dict);

    TspdfBuffer content = tspdf_buffer_create(4096);
    tx_load_page_content(&ctx, page->page_dict, &content);
    if (content.len > 0 && !content.error)
        tx_interpret(&ctx, content.data, content.len, &gs, 0);
    tspdf_buffer_destroy(&content);

    if (ctx.out.len > 0 && ctx.out.data[ctx.out.len - 1] != '\n')
        tspdf_buffer_append_byte(&ctx.out, '\n');

    char *result = NULL;
    if (!ctx.out.error) {
        result = (char *)tspdf_arena_alloc(&doc->arena, ctx.out.len + 1);
        if (result) {
            if (ctx.out.len) memcpy(result, ctx.out.data, ctx.out.len);
            result[ctx.out.len] = '\0';
        }
    }
    if (stats) {
        stats->glyphs = ctx.glyphs;
        stats->replacements = ctx.replacements;
    }
    tspdf_buffer_destroy(&ctx.out);
    tspdf_arena_destroy(&ctx.scratch);

    if (!result) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    if (err) *err = TSPDF_OK;
    return result;
}

const char *tspdf_reader_page_text(TspdfReader *doc, size_t page_index,
                                   TspdfError *err) {
    return tspdf_reader_page_text_stats(doc, page_index, NULL, err);
}
