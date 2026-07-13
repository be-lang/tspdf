// Content-stream text extraction (pdftotext-style, digitally-born PDFs).
//
// Decoding priority per font: /ToUnicode CMap (bfchar + bfrange, incl. the
// array form), then /Encoding name + /Differences (glyph names via a compact
// AGL subset), then StandardEncoding. Type0 fonts use 2-byte codes; without
// /ToUnicode each glyph becomes U+FFFD (counted in TspdfTextStats).
//
// Layout is heuristic and stays in content-stream order: a baseline more
// than 0.5 em off the current line's first run emits a newline (super- and
// subscripts stay on the line), a large x-gap within a line emits a space,
// and TJ adjustments beyond ~half the space width emit a space. Word-gap
// thresholds are clamped to [0.15, 0.20] em because declared space widths
// are unreliable: TeX subsets omit the space glyph entirely and TeX math
// fonts put a wide non-space glyph at code 32. Robustness
// beats fidelity: unknown operators are skipped, malformed CMaps degrade to
// the next fallback, and nothing here may crash on hostile input.

#include "tspr_internal.h"
#include "tspr_text.h"
#include "tspr_content_walk.h"
#include "../util/pdftext.h"
#include "../util/buffer.h"
#include "../pdf/pdf_base14.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEXT_MAX_DEPTH 8     // Form XObject recursion cap
#define TOU_MAX_UNITS 8      // UTF-16 units per ToUnicode target (ligatures)
// Total concatenated /Contents bytes per page. The per-stream decode cap
// alone lets a small file with many compressed streams demand N x 128 MB;
// past this limit extraction proceeds with what is already loaded.
#define TEXT_CONTENT_MAX ((size_t)256 << 20)
// Layout-mode character grid width. Column indexes derived from device-space
// x are clamped here so a hostile x=1e9 pads at most this many spaces
// instead of allocating by coordinate.
#define LAYOUT_MAX_COLS 1000

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
    uint32_t *tou_max_hi;       // running max of tou[0..i].hi (walk-back bound)
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

// Layout mode: one record per shown string run. The text bytes live in
// ctx->out (which serves as the fragment pool instead of the final output);
// geometry is device space.
typedef struct {
    size_t off, len;   // byte range in ctx->out
    double x, y;       // left edge and baseline
    double w;          // width (>= 0)
    double em;         // font size in device units
    size_t seq;        // content-stream order (stable sort tie-break)
    size_t line;       // content-order line id (tx_pre_run anchor rule)
} TextFrag;

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
    double line_y, line_em;  // current line's first run (baseline anchor)
    size_t line_id;          // content-order line counter (layout mode)
    bool layout;             // collect fragments instead of emitting inline
    TextFrag *frags;         // scratch-arena, doubling
    size_t nfrags, frag_cap;
} TextCtx;

typedef struct {
    TxMat ctm;
    TspdfObj *resources;
    TxMat tm, tlm;
    TextFont *font;
    double size, tc, tw, tl;
} TextGS;

// The walker owns the CTM and current /Resources and saves/restores them on
// q/Q; the text-specific graphics state (text matrices, font, spacing) lives
// in this client blob, which the walker saves/restores alongside. A full
// TextGS is reconstituted per operator from the walker's ctm/resources plus
// the blob (tx_gs_load) and the mutated text fields written back (tx_gs_store).
typedef struct {
    TxMat tm, tlm;
    TextFont *font;
    double size, tc, tw, tl;
} TextGSBlob;

static void tx_gs_load(TextGS *gs, const TextGSBlob *b, TxMat ctm,
                       TspdfObj *resources) {
    gs->ctm = ctm;
    gs->resources = resources;
    gs->tm = b->tm;
    gs->tlm = b->tlm;
    gs->font = b->font;
    gs->size = b->size;
    gs->tc = b->tc;
    gs->tw = b->tw;
    gs->tl = b->tl;
}

static void tx_gs_store(TextGSBlob *b, const TextGS *gs) {
    b->tm = gs->tm;
    b->tlm = gs->tlm;
    b->font = gs->font;
    b->size = gs->size;
    b->tc = gs->tc;
    b->tw = gs->tw;
    b->tl = gs->tl;
}

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
        if (slen >= TEXT_CONTENT_MAX - buf->len) { // includes the '\n' below
            free(bytes);
            break;
        }
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
        uint32_t *max_hi = (uint32_t *)tspdf_arena_alloc(&ctx->scratch,
                                                         list.n * sizeof(uint32_t));
        if (!max_hi) return; // no table beats a lookup that misses entries
        uint32_t run = 0;
        for (size_t i = 0; i < list.n; i++) {
            if (list.v[i].hi > run) run = list.v[i].hi;
            max_hi[i] = run;
        }
        f->tou = list.v;
        f->tou_max_hi = max_hi;
        f->tou_count = list.n;
        f->has_tou = true;
    }
}

static bool tou_entry_units(const ToUniEntry *e, uint32_t code,
                            uint16_t *units, uint8_t *n) {
    if (e->nunits == 0) return false;
    memcpy(units, e->units, e->nunits * sizeof(uint16_t));
    units[e->nunits - 1] = (uint16_t)(units[e->nunits - 1] + (code - e->lo));
    *n = e->nunits;
    return true;
}

static bool tou_lookup(const TextFont *f, uint32_t code,
                       uint16_t *units, uint8_t *n) {
    if (!f->tou_count) return false;
    // Binary search for the last entry with lo <= code; every entry before
    // idx has lo <= code, so a covering entry only needs hi >= code.
    size_t lo = 0, hi = f->tou_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (f->tou[mid].lo <= code) lo = mid + 1;
        else hi = mid;
    }
    size_t idx = lo;
    // Walk back most-specific-first (largest lo), e.g. bfchar point fixes
    // sitting inside a wide bfrange. tou_max_hi bounds the walk: once the
    // running max of hi drops below code, no earlier entry can cover it.
    for (int back = 0; idx > 0 && back < 8; back++) {
        if (f->tou_max_hi[idx - 1] < code) return false;
        const ToUniEntry *e = &f->tou[--idx];
        if (code <= e->hi) return tou_entry_units(e, code, units, n);
    }
    if (idx == 0 || f->tou_max_hi[idx - 1] < code) return false;
    // Still-covered code beyond the linear window (wide range with many
    // point fixes above it): binary-search the first index where the
    // running max reaches code — that entry's own hi produced the step,
    // so it covers code. Keeps hostile overlap-heavy CMaps O(log n).
    size_t blo = 0, bhi = idx;
    while (blo < bhi) {
        size_t mid = blo + (bhi - blo) / 2;
        if (f->tou_max_hi[mid] >= code) bhi = mid;
        else blo = mid + 1;
    }
    const ToUniEntry *e = &f->tou[blo];
    if (code < e->lo || code > e->hi) return false; // defensive; unreachable
    return tou_entry_units(e, code, units, n);
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

    // Space width: trust only real data (a /Widths entry for code 32 or
    // base-14 metrics). TeX-style subsets often start /Widths past the space
    // glyph (FirstChar 40) because inter-word gaps are TJ kerns; letting the
    // generic 500 default stand here doubles every word-gap threshold and
    // glues words at 0.25 em kerns. 0 = unknown; callers fall back to a
    // 250/1000-em nominal space.
    double sw = f->widths[32];
    if (sw <= 0 && b14 && b14->widths[32] > 0) sw = b14->widths[32];
    f->space_w = sw;

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
    // Skip U+0000 (pdftotext does the same): generators map .notdef glyphs
    // to <0000>, and an embedded NUL would truncate the returned C string,
    // hiding all following page text. Also reachable via bfrange arithmetic
    // wrapping a uint16 unit to 0.
    if (cp == 0) return;
    // Fold Latin ligature code points (U+FB00-FB06) to their ASCII letter
    // sequences so searching extracted text for "file" matches a PDF that
    // shows the ﬁ ligature. Sequences follow Unicode NFKC (and poppler's
    // Latin1 fold tables); note default pdftotext (UTF-8) keeps the raw
    // code points. U+FB05 is the long-s-t ligature: NFKC folds it to "st".
    if (cp >= 0xFB00 && cp <= 0xFB06) {
        static const char *const lig[7] = {"ff", "fi", "fl", "ffi", "ffl",
                                           "st", "st"};
        const char *s = lig[cp - 0xFB00];
        tspdf_buffer_append(&ctx->out, s, strlen(s));
        return;
    }
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

// Trim trailing spaces before ending a line (poppler behavior): generators
// pad lines with explicit space glyphs, and layout gap heuristics can add
// one more right before a baseline jump.
static void tx_trim_line_end(TextCtx *ctx) {
    while (ctx->out.len > 0 && ctx->out.data[ctx->out.len - 1] == ' ')
        ctx->out.len--;
}

static void tx_newline(TextCtx *ctx) {
    tx_trim_line_end(ctx);
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

// Record a layout-mode fragment: text bytes [off, out.len) shown from device
// x0 to x1 on baseline y. Non-finite geometry (hostile matrices) is dropped;
// the glyphs still counted toward stats but never placed.
static void tx_frag_push(TextCtx *ctx, size_t off, double x0, double x1,
                         double y, double em) {
    size_t len = ctx->out.len - off;
    if (len == 0) return;
    if (!isfinite(x0) || !isfinite(x1) || !isfinite(y)) return;
    if (ctx->nfrags >= ctx->frag_cap) {
        size_t new_cap = ctx->frag_cap == 0 ? 64 : ctx->frag_cap * 2;
        TextFrag *nv = (TextFrag *)tspdf_arena_alloc(&ctx->scratch,
                                                     new_cap * sizeof(TextFrag));
        if (!nv) return;
        if (ctx->frags && ctx->nfrags)
            memcpy(nv, ctx->frags, ctx->nfrags * sizeof(TextFrag));
        ctx->frags = nv;
        ctx->frag_cap = new_cap;
    }
    TextFrag *f = &ctx->frags[ctx->nfrags];
    f->off = off;
    f->len = len;
    f->x = x0 < x1 ? x0 : x1;
    f->y = y;
    f->w = fabs(x1 - x0);
    f->em = isfinite(em) && em > 0 ? em : 0;
    f->seq = ctx->nfrags;
    f->line = ctx->line_id;
    ctx->nfrags++;
}

// Compare the run start against the current line: a baseline more than
// 0.5 em off the line's first run emits a newline, an x-gap wider than
// ~half a space emits a space. 0.5 em is poppler's maxIntraLineDelta;
// anchoring at the line's first run instead of the previous run keeps
// super-/subscripts (10^20 exponents, footnote markers, both also on
// large headings, 0.35-0.50 em off the baseline) on the line and still
// lets a raised marker that *starts* a line (footnote text: small
// anchor, small tolerance) keep its own line, exactly like pdftotext.
// Real leading even in dense tables and footnotes stays above 0.8 em.
// Layout mode runs the same line bookkeeping (fragments carry the line id
// so assembly can group them in content order) but emits nothing here:
// spacing comes from fragment geometry.
static void tx_pre_run(TextCtx *ctx, const TextGS *gs) {
    double x, y, em, sx;
    tx_device_state(gs, &x, &y, &em, &sx);
    bool starts_line = true;
    if (ctx->have_last && ctx->out.len > 0) {
        double ref_em = em > ctx->last_em ? em : ctx->last_em;
        if (ref_em <= 0) ref_em = 1.0;
        double anchor_em = ctx->line_em > 0 ? ctx->line_em : ref_em;
        double dy = fabs(y - ctx->line_y);
        double dx = x - ctx->last_x;
        if (dy > 0.5 * anchor_em) {
            if (!ctx->layout) tx_newline(ctx);
        } else {
            starts_line = false;
            double size = gs->size != 0 ? fabs(gs->size) : 1.0;
            double space_w = gs->font && gs->font->space_w > 0 ? gs->font->space_w : 250;
            double thr = 0.5 * (space_w / 1000.0) * size * sx;
            // Clamp to [0.15, 0.20] em: poppler breaks words near 0.1 em
            // regardless of the font, and declared space widths are not
            // reliable — TeX math fonts carry a real glyph at code 32
            // (cmmi: 651/1000) whose width would push the threshold past
            // a normal 0.25 em word gap and glue words together at
            // font-change boundaries ("of dk", "with dmodel").
            double min_thr = 0.15 * ref_em;
            double max_thr = 0.20 * ref_em;
            if (thr < min_thr) thr = min_thr;
            if (thr > max_thr) thr = max_thr;
            // Super-/subscript boundary (the baseline moved but stayed on
            // the line): word gaps here are set in the smaller script size,
            // so use poppler's minWordBreakSpace, 0.1 of the smaller em.
            // Keeps a raised "T" a word ("QK T", pdftotext does the same)
            // while an exponent or marker drawn flush against its word
            // ("Vaswani*", "1.0-10" + "20" -> "1.0-1020") stays glued.
            double dy_prev = fabs(y - ctx->last_y);
            if (dy_prev > 0.1 * ref_em) {
                double small_em = em > 0 && (ctx->last_em <= 0 || em < ctx->last_em)
                                      ? em : ctx->last_em;
                if (small_em > 0) thr = 0.1 * small_em;
            }
            if (!ctx->layout && (dx > thr || dx < -ref_em)) tx_space(ctx);
        }
    }
    if (starts_line) {
        ctx->line_y = y;
        ctx->line_em = em;
        ctx->line_id++;
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

    double run_x = 0, run_y = 0, run_em = 0, run_sx = 0;
    size_t run_off = 0;
    if (ctx->layout) {
        tx_device_state(gs, &run_x, &run_y, &run_em, &run_sx);
        run_off = ctx->out.len;
    }

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

    if (ctx->layout) {
        double end_x, end_y, end_em, end_sx;
        tx_device_state(gs, &end_x, &end_y, &end_em, &end_sx);
        tx_frag_push(ctx, run_off, run_x, end_x, run_y, run_em);
    }
}

// --- Interpreter (driven by the shared content-stream walker) ---

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

// Walker load_stream/free callbacks.
static uint8_t *tx_walk_load(void *client, TspdfObj *s, uint32_t obj_num,
                             size_t *out_len) {
    return tx_load_stream((TextCtx *)client, s, obj_num, out_len);
}

static void tx_walk_free(void *client, uint8_t *bytes) {
    (void)client;
    free(bytes);
}

// A Form XObject was entered: reset the text matrices for its sub-stream, like
// a fresh BT would (the walker gives us a private blob copy to mutate).
static void tx_walk_form_enter(void *client, void *gstate, TspdfObj *form) {
    (void)client;
    (void)form;
    TextGSBlob *b = (TextGSBlob *)gstate;
    b->tm = b->tlm = TX_IDENTITY;
}

// Operator callback: the walker already handled q/Q/cm/Do/BI/true/false/null,
// so this sees only the text operators. Reconstitute a full TextGS from the
// walker's ctm/resources plus the client blob, run the operator, and store the
// mutated text state back into the blob.
static void tx_walk_op(void *client, const char *op, TspdfObj **stk, int ns,
                       TspdfWalkMat wctm, TspdfObj *resources, void *gstate) {
    TextCtx *ctx = (TextCtx *)client;
    TextGSBlob *b = (TextGSBlob *)gstate;
    TxMat ctm = {wctm.a, wctm.b, wctm.c, wctm.d, wctm.e, wctm.f};
    TextGS gs;
    tx_gs_load(&gs, b, ctm, resources);

    if (strcmp(op, "BT") == 0) {
        gs.tm = gs.tlm = TX_IDENTITY;
    } else if (strcmp(op, "ET") == 0) {
        // nothing
    } else if (strcmp(op, "Tf") == 0) {
        gs.size = stk_num(stk, ns, 0);
        gs.font = tx_get_font(ctx, gs.resources, stk_at(stk, ns, 1));
    } else if (strcmp(op, "Td") == 0) {
        tx_op_td(&gs, stk_num(stk, ns, 1), stk_num(stk, ns, 0));
    } else if (strcmp(op, "TD") == 0) {
        gs.tl = -stk_num(stk, ns, 0);
        tx_op_td(&gs, stk_num(stk, ns, 1), stk_num(stk, ns, 0));
    } else if (strcmp(op, "Tm") == 0) {
        TxMat m;
        m.a = stk_num(stk, ns, 5);
        m.b = stk_num(stk, ns, 4);
        m.c = stk_num(stk, ns, 3);
        m.d = stk_num(stk, ns, 2);
        m.e = stk_num(stk, ns, 1);
        m.f = stk_num(stk, ns, 0);
        gs.tm = gs.tlm = m;
    } else if (strcmp(op, "T*") == 0) {
        tx_op_td(&gs, 0, -gs.tl);
    } else if (strcmp(op, "TL") == 0) {
        gs.tl = stk_num(stk, ns, 0);
    } else if (strcmp(op, "Tc") == 0) {
        gs.tc = stk_num(stk, ns, 0);
    } else if (strcmp(op, "Tw") == 0) {
        gs.tw = stk_num(stk, ns, 0);
    } else if (strcmp(op, "Tz") == 0 || strcmp(op, "Ts") == 0 ||
               strcmp(op, "Tr") == 0) {
        // horizontal scale / rise / render mode: explicitly ignored
    } else if (strcmp(op, "Tj") == 0) {
        tx_pre_run(ctx, &gs);
        tx_show_string(ctx, &gs, stk_at(stk, ns, 0));
        tx_post_run(ctx, &gs);
    } else if (strcmp(op, "TJ") == 0) {
        TspdfObj *arr = stk_at(stk, ns, 0);
        if (arr && arr->type == TSPDF_OBJ_ARRAY) {
            tx_pre_run(ctx, &gs);
            double thr = gs.font && gs.font->space_w > 0
                             ? gs.font->space_w * 0.5 : 120.0;
            if (thr > 200.0) thr = 200.0; // same 0.20 em cap as tx_pre_run
            for (size_t i = 0; i < arr->array.count; i++) {
                TspdfObj *el = &arr->array.items[i];
                if (el->type == TSPDF_OBJ_STRING) {
                    tx_show_string(ctx, &gs, el);
                } else if (el->type == TSPDF_OBJ_INT ||
                           el->type == TSPDF_OBJ_REAL) {
                    double v = tx_obj_num(el);
                    if (!ctx->layout && -v > thr) tx_space(ctx);
                    gs.tm = tx_mul(tx_translate(-v / 1000.0 * gs.size, 0),
                                   gs.tm);
                }
            }
            tx_post_run(ctx, &gs);
        }
    } else if (strcmp(op, "'") == 0) {
        tx_op_td(&gs, 0, -gs.tl);
        tx_pre_run(ctx, &gs);
        tx_show_string(ctx, &gs, stk_at(stk, ns, 0));
        tx_post_run(ctx, &gs);
    } else if (strcmp(op, "\"") == 0) {
        gs.tw = stk_num(stk, ns, 2);
        gs.tc = stk_num(stk, ns, 1);
        tx_op_td(&gs, 0, -gs.tl);
        tx_pre_run(ctx, &gs);
        tx_show_string(ctx, &gs, stk_at(stk, ns, 0));
        tx_post_run(ctx, &gs);
    }
    // every other operator: skipped

    tx_gs_store(b, &gs);
}

// Configure a walker for text extraction: full text graphics-state blob, an
// operator callback, no image callback (text ignores images).
static void tx_walker_init(TextCtx *ctx, TspdfWalker *w) {
    memset(w, 0, sizeof(*w));
    w->doc = ctx->doc;
    w->rp = &ctx->rp;
    w->scratch = &ctx->scratch;
    w->client = ctx;
    w->max_depth = TEXT_MAX_DEPTH;
    w->gstate_size = sizeof(TextGSBlob);
    w->load_stream = tx_walk_load;
    w->load_stream_free = tx_walk_free;
    w->op = tx_walk_op;
    w->form_enter = tx_walk_form_enter;
}

// --- Layout assembly (fragments -> character grid) ---

static size_t tx_utf8_chars(const uint8_t *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if ((s[i] & 0xC0) != 0x80) c++;
    return c;
}

// One content-order line: a contiguous fragment range (fragments are
// recorded in content order and line ids never decrease) plus baseline
// aggregates. y/em are the line's first fragment — the anchor tx_pre_run
// grouped the line around.
typedef struct {
    size_t start, end;   // fragment index range [start, end)
    double y, em;        // anchor baseline and font size
    double y_min, y_max; // baseline extremes across the line
    double em_max;
} TextLine;

// Anchor baseline descending (top of page first), content order on ties.
static int line_cmp_y(const void *a, const void *b) {
    const TextLine *la = (const TextLine *)a, *lb = (const TextLine *)b;
    if (la->y > lb->y) return -1;
    if (la->y < lb->y) return 1;
    return la->start < lb->start ? -1 : la->start > lb->start ? 1 : 0;
}

static int frag_cmp_x(const void *a, const void *b) {
    const TextFrag *fa = (const TextFrag *)a, *fb = (const TextFrag *)b;
    if (fa->x < fb->x) return -1;
    if (fa->x > fb->x) return 1;
    return fa->seq < fb->seq ? -1 : fa->seq > fb->seq ? 1 : 0;
}

typedef struct { double adv; size_t chars; } AdvSample;

static int adv_cmp(const void *a, const void *b) {
    double da = ((const AdvSample *)a)->adv, db = ((const AdvSample *)b)->adv;
    return da < db ? -1 : da > db ? 1 : 0;
}

// Grid cell width: the character-weighted median of each fragment's average
// advance (device-space width / character count). Robust against a few huge
// or zero-width outliers; computed per page, never hardcoded.
static double tx_layout_char_width(TextCtx *ctx) {
    AdvSample *s = (AdvSample *)tspdf_arena_alloc(&ctx->scratch,
                                                  ctx->nfrags * sizeof(AdvSample));
    double fallback_em = 0;
    size_t n = 0, total = 0;
    for (size_t i = 0; i < ctx->nfrags; i++) {
        const TextFrag *f = &ctx->frags[i];
        if (f->em > fallback_em) fallback_em = f->em;
        size_t chars = tx_utf8_chars(ctx->out.data + f->off, f->len);
        if (!s || chars == 0 || f->w <= 0 || !isfinite(f->w)) continue;
        double adv = f->w / (double)chars;
        if (adv <= 0 || !isfinite(adv)) continue;
        s[n].adv = adv;
        s[n].chars = chars;
        n++;
        total += chars;
    }
    double cw;
    if (n > 0) {
        qsort(s, n, sizeof(AdvSample), adv_cmp);
        size_t half = (total + 1) / 2, cum = 0, i = 0;
        for (; i + 1 < n; i++) {
            cum += s[i].chars;
            if (cum >= half) break;
        }
        cw = s[i].adv;
    } else {
        cw = fallback_em > 0 ? 0.5 * fallback_em : 6.0;
    }
    if (!(cw >= 0.1)) cw = 0.1;      // also catches NaN
    if (cw > 1e6) cw = 1e6;
    return cw;
}

// Place fragments on a character grid, top row first. pdftotext-layout
// architecture in miniature: fragments were grouped into content-order
// lines during interpretation (0.5 em anchor rule, so super-/subscripts
// belong to the line that was already open), and here whole lines merge
// into one visual row when a lower line's anchor baseline is within
// 0.5 em (poppler's maxIntraLineDelta) of the row's top line anchor.
// That covers y jitter and table rows whose cell columns use different
// leading and drift a few points apart (the syscard corpus), while a
// raised footnote marker that opened its own tiny-anchor line keeps its
// own row, exactly like pdftotext -layout. Real leading stays above
// 0.8 em. Column = round((x - page_min_x) / char_width), clamped to the
// grid cap. Fragments in a row go left to right; padding spaces carry
// them to their column, with at least one space kept between separated
// fragments and a single space when fragments overlap. Vertical gaps
// beyond ~1.8 line heights, measured from the previous row's lowest
// baseline so a merged multi-baseline row does not open a gap by
// itself, become blank lines (at most 2). Result is appended to `dst`.
static void tx_layout_assemble(TextCtx *ctx, TspdfBuffer *dst) {
    size_t n = ctx->nfrags;
    if (n == 0) return;
    TextFrag *frags = ctx->frags;

    double cw = tx_layout_char_width(ctx);
    double min_x = frags[0].x;
    for (size_t i = 1; i < n; i++)
        if (frags[i].x < min_x) min_x = frags[i].x;

    TextLine *lines = (TextLine *)tspdf_arena_alloc(&ctx->scratch,
                                                    n * sizeof(TextLine));
    TextFrag *row = (TextFrag *)tspdf_arena_alloc(&ctx->scratch,
                                                  n * sizeof(TextFrag));
    if (!lines || !row) return;
    size_t nlines = 0;
    for (size_t i = 0; i < n; i++) {
        if (nlines == 0 || frags[i].line != frags[lines[nlines - 1].start].line) {
            TextLine *nl = &lines[nlines++];
            nl->start = i;
            nl->y = nl->y_min = nl->y_max = frags[i].y;
            nl->em = nl->em_max = frags[i].em;
        }
        TextLine *l = &lines[nlines - 1];
        l->end = i + 1;
        if (frags[i].y < l->y_min) l->y_min = frags[i].y;
        if (frags[i].y > l->y_max) l->y_max = frags[i].y;
        if (frags[i].em > l->em_max) l->em_max = frags[i].em;
    }
    qsort(lines, nlines, sizeof(TextLine), line_cmp_y);

    double prev_y = 0, prev_em = 0;
    bool have_prev = false;
    size_t li = 0;
    while (li < nlines) {
        double row_tol = lines[li].em > 0 ? 0.5 * lines[li].em : 2.0;
        double row_top = lines[li].y_max;
        double row_bottom = lines[li].y_min;
        double row_em = lines[li].em_max;
        size_t lj = li + 1;
        while (lj < nlines && lines[li].y - lines[lj].y <= row_tol) {
            if (lines[lj].y_max > row_top) row_top = lines[lj].y_max;
            if (lines[lj].y_min < row_bottom) row_bottom = lines[lj].y_min;
            if (lines[lj].em_max > row_em) row_em = lines[lj].em_max;
            lj++;
        }
        size_t rn = 0;
        for (size_t l = li; l < lj; l++)
            for (size_t i = lines[l].start; i < lines[l].end; i++)
                row[rn++] = frags[i];
        qsort(row, rn, sizeof(TextFrag), frag_cmp_x);

        if (have_prev) {
            double lh = prev_em > row_em ? prev_em : row_em;
            if (lh > 0) {
                double gap = prev_y - row_top;
                if (gap > 1.8 * lh)
                    tspdf_buffer_append(dst, gap > 3.2 * lh ? "\n\n" : "\n",
                                        gap > 3.2 * lh ? 2 : 1);
            }
        }

        size_t line_start = dst->len;
        size_t cur_col = 0;
        double cur_end = 0;
        for (size_t k = 0; k < rn; k++) {
            const TextFrag *f = &row[k];
            double rel = (f->x - min_x) / cw;
            size_t col = rel <= 0 ? 0
                       : rel >= (double)(LAYOUT_MAX_COLS - 1) ? LAYOUT_MAX_COLS - 1
                       : (size_t)(rel + 0.5);
            size_t pad;
            if (k == 0) {
                pad = col;
            } else {
                double gap = f->x - cur_end;
                if (gap >= 0.3 * cw)
                    pad = col > cur_col ? col - cur_col : 1; // keep a real gap
                else if (gap > -cw)
                    pad = 0;                                 // contiguous run
                else
                    pad = 1;                                 // overlap: merge
                // Super-/subscript boundary within the row: a small but
                // real gap is a word break at the script size (poppler's
                // minWordBreakSpace, 0.1 em of the smaller fragment).
                if (pad == 0 && gap > 0) {
                    const TextFrag *pf = &row[k - 1];
                    double big_em = f->em > pf->em ? f->em : pf->em;
                    double small_em = f->em > 0 && (pf->em <= 0 || f->em < pf->em)
                                          ? f->em : pf->em;
                    if (big_em > 0 && small_em > 0 &&
                        fabs(f->y - pf->y) > 0.1 * big_em &&
                        gap > 0.1 * small_em)
                        pad = 1;
                }
            }
            if (pad > LAYOUT_MAX_COLS) pad = LAYOUT_MAX_COLS;
            for (size_t sp = 0; sp < pad; sp++)
                tspdf_buffer_append_byte(dst, ' ');
            tspdf_buffer_append(dst, ctx->out.data + f->off, f->len);
            cur_col += pad + tx_utf8_chars(ctx->out.data + f->off, f->len);
            double end = f->x + f->w;
            if (k == 0 || end > cur_end) cur_end = end;
        }
        while (dst->len > line_start && dst->data[dst->len - 1] == ' ')
            dst->len--;
        tspdf_buffer_append_byte(dst, '\n');

        have_prev = true;
        prev_y = row_bottom;
        prev_em = row_em;
        li = lj;
    }
}

// --- Public API ---

static const char *tx_page_text(TspdfReader *doc, size_t page_index, bool layout,
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
    ctx.layout = layout;
    tspdf_parser_init(&ctx.rp, doc->data, doc->data_len, &doc->arena);
    ctx.scratch = tspdf_arena_create(65536);
    if (!ctx.scratch.first) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    ctx.out = tspdf_buffer_create(4096);

    TspdfReaderPage *page = &doc->pages.pages[page_index];
    TspdfObj *resources = tx_page_resources(&ctx, page->page_dict);
    TextGSBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.tm = blob.tlm = TX_IDENTITY;

    TspdfBuffer content = tspdf_buffer_create(4096);
    tx_load_page_content(&ctx, page->page_dict, &content);
    if (content.len > 0 && !content.error) {
        TspdfWalker walker;
        tx_walker_init(&ctx, &walker);
        tspdf_walk_run(&walker, content.data, content.len, TSPDF_WALK_IDENTITY,
                       resources, 0, false, &blob);
    }
    tspdf_buffer_destroy(&content);

    if (layout) {
        // ctx.out held the fragment pool; assemble the grid and swap it in.
        TspdfBuffer assembled = tspdf_buffer_create(ctx.out.len + 256);
        if (!ctx.out.error) tx_layout_assemble(&ctx, &assembled);
        else assembled.error = true;
        tspdf_buffer_destroy(&ctx.out);
        ctx.out = assembled;
    } else {
        tx_trim_line_end(&ctx);
        if (ctx.out.len > 0 && ctx.out.data[ctx.out.len - 1] != '\n')
            tspdf_buffer_append_byte(&ctx.out, '\n');
    }

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

const char *tspdf_reader_page_text_stats(TspdfReader *doc, size_t page_index,
                                         TspdfTextStats *stats, TspdfError *err) {
    return tx_page_text(doc, page_index, false, stats, err);
}

const char *tspdf_reader_page_text(TspdfReader *doc, size_t page_index,
                                   TspdfError *err) {
    return tx_page_text(doc, page_index, false, NULL, err);
}

const char *tspdf_reader_page_text_layout_stats(TspdfReader *doc, size_t page_index,
                                                TspdfTextStats *stats, TspdfError *err) {
    return tx_page_text(doc, page_index, true, stats, err);
}

const char *tspdf_reader_page_text_layout(TspdfReader *doc, size_t page_index,
                                          TspdfError *err) {
    return tx_page_text(doc, page_index, true, NULL, err);
}
