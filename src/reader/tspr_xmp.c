// XMP metadata sync: apply pending Info-dict edits to the XMP packet in the
// catalog /Metadata stream (tspdf_reader_sync_xmp_metadata, tspr.h).
//
// This is deliberately NOT an XML parser. XMP packets are structured enough
// for conservative textual editing: when a property already exists — element
// form (<dc:title>...</dc:title>, with the rdf:Alt/rdf:Seq item forms for the
// Dublin Core properties) or attribute form on rdf:Description — its VALUE is
// replaced with proper XML escaping. Nothing is ever injected: a property
// that is absent, holds more structure than one value (multi-author
// dc:creator), or lives in a packet we cannot edit safely (UTF-16, filters we
// cannot decode) is reported back so the caller can warn instead. The xpacket
// padding before the <?xpacket end="w"?> trailer absorbs length changes; when
// a value outgrows it the stream grows (the file is rewritten anyway) and the
// trailer is kept.
#define _POSIX_C_SOURCE 200809L
#include "tspr_internal.h"
#include "tspr_doctree.h"
#include "../util/pdftext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_DC "http://purl.org/dc/elements/1.1/"
#define NS_PDF "http://ns.adobe.com/pdf/1.3/"
#define NS_XMP "http://ns.adobe.com/xap/1.0/"
#define NS_RDF "http://www.w3.org/1999/02/22-rdf-syntax-ns#"

typedef enum { XMP_SIMPLE, XMP_ALT, XMP_SEQ } XmpForm;

typedef struct {
    const char *uri;             // namespace the property belongs to
    const char *fallback_prefix; // conventional prefix, tried after declared ones
    const char *local;           // local name
    XmpForm form;
} XmpProp;

static const XmpProp PROP_TITLE = {NS_DC, "dc", "title", XMP_ALT};
static const XmpProp PROP_AUTHOR = {NS_DC, "dc", "creator", XMP_SEQ};
static const XmpProp PROP_SUBJECT = {NS_DC, "dc", "description", XMP_ALT};
static const XmpProp PROP_KEYWORDS = {NS_PDF, "pdf", "Keywords", XMP_SIMPLE};
static const XmpProp PROP_CREATOR = {NS_XMP, "xmp", "CreatorTool", XMP_SIMPLE};
static const XmpProp PROP_PRODUCER = {NS_PDF, "pdf", "Producer", XMP_SIMPLE};
static const XmpProp PROP_MODIFY_DATE = {NS_XMP, "xmp", "ModifyDate", XMP_SIMPLE};

// --- Small growable buffer holding the packet under edit ---

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} XBuf;

// Replace [start,end) with `n` bytes of `ins`.
static bool xb_splice(XBuf *b, size_t start, size_t end, const char *ins, size_t n) {
    if (start > end || end > b->len) return false;
    size_t new_len = b->len - (end - start) + n;
    if (new_len > b->cap) {
        size_t nc = new_len * 2;
        uint8_t *nd = (uint8_t *)realloc(b->data, nc);
        if (!nd) return false;
        b->data = nd;
        b->cap = nc;
    }
    memmove(b->data + start + n, b->data + end, b->len - end);
    if (n > 0) memcpy(b->data + start, ins, n);
    b->len = new_len;
    return true;
}

// --- Text search helpers ---

static bool is_xml_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Find `needle` in h[from..limit); SIZE_MAX when absent.
static size_t xfind(const uint8_t *h, size_t limit, size_t from, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || from >= limit || nl > limit - from) return SIZE_MAX;
    for (size_t i = from; i + nl <= limit; i++) {
        if (h[i] == (uint8_t)needle[0] && memcmp(h + i, needle, nl) == 0) return i;
    }
    return SIZE_MAX;
}

// --- Element location ---

typedef struct {
    size_t open_start;    // '<' of the open tag
    size_t open_end;      // just past its '>'
    bool empty;           // <name .../>
    size_t content_start; // == open_end (undefined when empty)
    size_t content_end;   // '<' of the closing tag (undefined when empty)
    size_t elem_end;      // just past the closing '>' (== open_end when empty)
} XElem;

// Find element <prefix:local ...> ... </prefix:local> within [from, limit).
// Whitespace-tolerant: any run of blanks/newlines may follow the tag name.
static bool xmp_find_element(const uint8_t *p, size_t limit, size_t from,
                             const char *prefix, const char *local, XElem *e) {
    char name[80];
    if (snprintf(name, sizeof(name), "%s:%s", prefix, local) >= (int)sizeof(name)) {
        return false;
    }
    size_t name_len = strlen(name);
    char open_pat[84];
    snprintf(open_pat, sizeof(open_pat), "<%s", name);

    size_t pos = from;
    for (;;) {
        size_t s = xfind(p, limit, pos, open_pat);
        if (s == SIZE_MAX) return false;
        size_t after = s + 1 + name_len;
        if (after >= limit) return false;
        uint8_t c = p[after];
        if (!(is_xml_ws(c) || c == '>' || c == '/')) {
            pos = s + 1; // e.g. <dc:titlefoo — keep scanning
            continue;
        }
        size_t gt = after;
        while (gt < limit && p[gt] != '>') gt++;
        if (gt >= limit) return false;
        e->open_start = s;
        e->open_end = gt + 1;
        e->empty = p[gt - 1] == '/';
        if (e->empty) {
            e->content_start = e->content_end = e->elem_end = gt + 1;
            return true;
        }
        char close_pat[84];
        snprintf(close_pat, sizeof(close_pat), "</%s", name);
        size_t ce = xfind(p, limit, gt + 1, close_pat);
        if (ce == SIZE_MAX) return false;
        size_t cgt = ce + 2 + name_len;
        while (cgt < limit && is_xml_ws(p[cgt])) cgt++;
        if (cgt >= limit || p[cgt] != '>') return false;
        e->content_start = gt + 1;
        e->content_end = ce;
        e->elem_end = cgt + 1;
        return true;
    }
}

// --- Value escaping ---

// Escape `v` for use as XML element text or a quoted attribute value.
// Returns malloc'd bytes, or NULL when the value cannot be XML text at all
// (invalid UTF-8, or control characters other than tab/newline/CR).
static char *xmp_escape(const char *v) {
    size_t cap = strlen(v) * 6 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    const char *s = v;
    while (*s) {
        uint32_t cp = 0;
        size_t adv = tspdf_utf8_decode(s, &cp);
        if (adv == 0 || (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r')) {
            free(out);
            return NULL;
        }
        switch (cp) {
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(out + o, "&apos;", 6); o += 6; break;
        default: memcpy(out + o, s, adv); o += adv; break;
        }
        s += adv;
    }
    out[o] = '\0';
    return out;
}

// --- Namespace prefix discovery ---

#define XMP_MAX_PREFIX 24
#define XMP_MAX_PREFIXES 4

// Collect the prefixes bound to `uri` by xmlns:PFX="uri" declarations.
static size_t xmp_prefixes(const uint8_t *p, size_t n, const char *uri,
                           char out[][XMP_MAX_PREFIX], size_t max) {
    size_t count = 0;
    size_t ulen = strlen(uri);
    size_t pos = 0;
    while (count < max) {
        size_t s = xfind(p, n, pos, "xmlns:");
        if (s == SIZE_MAX) break;
        pos = s + 6;
        size_t e = pos;
        while (e < n && p[e] != '=' && !is_xml_ws(p[e]) &&
               e - pos < XMP_MAX_PREFIX - 1) {
            e++;
        }
        size_t plen = e - pos;
        if (plen == 0 || plen >= XMP_MAX_PREFIX - 1) continue;
        size_t j = e;
        while (j < n && is_xml_ws(p[j])) j++;
        if (j >= n || p[j] != '=') continue;
        j++;
        while (j < n && is_xml_ws(p[j])) j++;
        if (j >= n || (p[j] != '"' && p[j] != '\'')) continue;
        uint8_t q = p[j];
        j++;
        if (j + ulen >= n || memcmp(p + j, uri, ulen) != 0 || p[j + ulen] != q) {
            continue;
        }
        char tmp[XMP_MAX_PREFIX];
        memcpy(tmp, p + pos, plen);
        tmp[plen] = '\0';
        bool dup = false;
        for (size_t k = 0; k < count; k++) {
            if (strcmp(out[k], tmp) == 0) dup = true;
        }
        if (!dup) {
            memcpy(out[count], tmp, plen + 1);
            count++;
        }
    }
    return count;
}

// --- Per-form editors ---

enum { X_NOT_FOUND = 0, X_EDITED = 1, X_BAIL = 2 };

// Replace an element's value: <p:l>old</p:l> -> <p:l>esc</p:l>, and expand
// the empty form <p:l .../> to <p:l ...>esc</p:l> (attributes preserved).
// `e` must describe the element within b->data.
static int xmp_replace_elem_value(XBuf *b, const XElem *e, const char *prefix,
                                  const char *local, const char *esc) {
    if (e->empty) {
        // b[open_start .. open_end-2) is the tag minus "/>".
        size_t head_len = e->open_end - 2 - e->open_start;
        size_t rep_len = head_len + 1 + strlen(esc) + strlen(prefix) + strlen(local) + 4;
        char *rep = (char *)malloc(rep_len + 1);
        if (!rep) return X_BAIL;
        memcpy(rep, b->data + e->open_start, head_len);
        // Drop a blank left before the '/' (e.g. "<a />") for tidy output.
        while (head_len > 0 && is_xml_ws((uint8_t)rep[head_len - 1])) head_len--;
        int n = snprintf(rep + head_len, rep_len + 1 - head_len, ">%s</%s:%s>",
                         esc, prefix, local);
        bool ok = n > 0 &&
                  xb_splice(b, e->open_start, e->open_end, rep, head_len + (size_t)n);
        free(rep);
        return ok ? X_EDITED : X_BAIL;
    }
    // Value replacement only: nested markup means more structure than a value.
    if (memchr(b->data + e->content_start, '<',
               e->content_end - e->content_start) != NULL) {
        return X_BAIL;
    }
    return xb_splice(b, e->content_start, e->content_end, esc, strlen(esc))
               ? X_EDITED
               : X_BAIL;
}

// Simple text property in element form.
static int xmp_edit_simple_elem(XBuf *b, const char *prefix, const char *local,
                                const char *esc) {
    XElem e;
    if (!xmp_find_element(b->data, b->len, 0, prefix, local, &e)) {
        return X_NOT_FOUND;
    }
    return xmp_replace_elem_value(b, &e, prefix, local, esc);
}

// Dublin Core container property: <p:l><rdf:Alt|Seq><rdf:li>value</rdf:li>...
// For Alt the x-default item is edited (any item if it is the only one); for
// Seq exactly one item must exist — several authors cannot be represented by
// the single Info string, so that bails.
static int xmp_edit_li(XBuf *b, const char *prefix, const char *local,
                       const char *rdf_prefix, bool alt, const char *esc) {
    XElem e;
    if (!xmp_find_element(b->data, b->len, 0, prefix, local, &e)) {
        return X_NOT_FOUND;
    }
    if (e.empty) return X_BAIL; // no item to hold the value; never inject

    XElem li[3];
    size_t count = 0;
    size_t pos = e.content_start;
    while (count < 3 &&
           xmp_find_element(b->data, e.content_end, pos, rdf_prefix, "li",
                            &li[count])) {
        pos = li[count].elem_end;
        count++;
    }
    if (count == 0) return X_BAIL;

    const XElem *chosen = NULL;
    if (alt) {
        for (size_t i = 0; i < count; i++) {
            const uint8_t *tag = b->data + li[i].open_start;
            size_t tag_len = li[i].open_end - li[i].open_start;
            if (xfind(tag, tag_len, 0, "x-default") != SIZE_MAX) {
                chosen = &li[i];
                break;
            }
        }
    }
    if (!chosen && count == 1) chosen = &li[0];
    if (!chosen) return X_BAIL;
    return xmp_replace_elem_value(b, chosen, rdf_prefix, "li", esc);
}

// Attribute form on rdf:Description: prefix:local="value".
static int xmp_edit_attr(XBuf *b, const char *prefix, const char *local,
                         const char *esc) {
    char name[80];
    if (snprintf(name, sizeof(name), "%s:%s", prefix, local) >= (int)sizeof(name)) {
        return X_NOT_FOUND;
    }
    size_t nlen = strlen(name);
    for (size_t i = 1; i + nlen < b->len; i++) {
        if (!is_xml_ws(b->data[i - 1])) continue;
        if (memcmp(b->data + i, name, nlen) != 0) continue;
        size_t j = i + nlen;
        while (j < b->len && is_xml_ws(b->data[j])) j++;
        if (j >= b->len || b->data[j] != '=') continue;
        j++;
        while (j < b->len && is_xml_ws(b->data[j])) j++;
        if (j >= b->len || (b->data[j] != '"' && b->data[j] != '\'')) continue;
        uint8_t q = b->data[j];
        size_t vs = j + 1;
        size_t ve = vs;
        while (ve < b->len && b->data[ve] != q) ve++;
        if (ve >= b->len) return X_BAIL;
        // Must sit inside a start tag, not in element text.
        bool in_tag = false;
        for (size_t k = i; k-- > 0;) {
            if (b->data[k] == '>') break;
            if (b->data[k] == '<') {
                in_tag = k + 1 < b->len && b->data[k + 1] != '/' &&
                         b->data[k + 1] != '?' && b->data[k + 1] != '!';
                break;
            }
        }
        if (!in_tag) continue;
        return xb_splice(b, vs, ve, esc, strlen(esc)) ? X_EDITED : X_BAIL;
    }
    return X_NOT_FOUND;
}

// Try to apply one field. True when the packet now carries the new value.
static bool xmp_apply_field(XBuf *b, const XmpProp *prop, const char *value) {
    char *esc = xmp_escape(value ? value : "");
    if (!esc) return false;

    char prefixes[XMP_MAX_PREFIXES + 1][XMP_MAX_PREFIX];
    size_t np = xmp_prefixes(b->data, b->len, prop->uri, prefixes, XMP_MAX_PREFIXES);
    bool have_fallback = false;
    for (size_t i = 0; i < np; i++) {
        if (strcmp(prefixes[i], prop->fallback_prefix) == 0) have_fallback = true;
    }
    if (!have_fallback) {
        snprintf(prefixes[np], XMP_MAX_PREFIX, "%s", prop->fallback_prefix);
        np++;
    }

    char rdf_prefixes[1][XMP_MAX_PREFIX];
    const char *rdf = "rdf";
    if (xmp_prefixes(b->data, b->len, NS_RDF, rdf_prefixes, 1) == 1) {
        rdf = rdf_prefixes[0];
    }

    int result = X_NOT_FOUND;
    for (size_t i = 0; i < np && result == X_NOT_FOUND; i++) {
        if (prop->form == XMP_SIMPLE) {
            result = xmp_edit_simple_elem(b, prefixes[i], prop->local, esc);
        } else {
            result = xmp_edit_li(b, prefixes[i], prop->local, rdf,
                                 prop->form == XMP_ALT, esc);
        }
        if (result == X_NOT_FOUND) {
            result = xmp_edit_attr(b, prefixes[i], prop->local, esc);
        }
    }
    free(esc);
    return result == X_EDITED;
}

// --- Packet plumbing ---

// Only UTF-8 packets are editable (XMP allows UTF-16/32; those carry NUL
// bytes for ASCII markup). The xpacket begin attribute, when present, must be
// empty or the UTF-8 BOM.
static bool xmp_packet_editable(const uint8_t *p, size_t n) {
    if (n == 0 || memchr(p, '\0', n) != NULL) return false;
    size_t h = xfind(p, n, 0, "<?xpacket begin=");
    if (h == SIZE_MAX) return true; // headerless packets exist; still UTF-8
    size_t j = h + strlen("<?xpacket begin=");
    if (j >= n || (p[j] != '"' && p[j] != '\'')) return false;
    uint8_t q = p[j];
    j++;
    if (j < n && p[j] == q) return true; // begin=""
    if (j + 3 < n && p[j] == 0xEF && p[j + 1] == 0xBB && p[j + 2] == 0xBF &&
        p[j + 3] == q) {
        return true;
    }
    return false;
}

// Re-establish the xpacket padding contract after edits: the whitespace run
// before the <?xpacket end trailer absorbs the length change so the packet
// keeps its original size; when the content outgrew it, the stream grows to
// a fresh 2 KiB pad instead (the file is rewritten, /Length follows).
static void xmp_fix_padding(XBuf *b, size_t target_len) {
    size_t t = SIZE_MAX;
    size_t pos = 0;
    for (;;) {
        size_t s = xfind(b->data, b->len, pos, "<?xpacket end=");
        if (s == SIZE_MAX) break;
        t = s;
        pos = s + 1;
    }
    if (t == SIZE_MAX) return; // no trailer, no padding contract
    size_t ps = t;
    while (ps > 0 && is_xml_ws(b->data[ps - 1])) ps--;
    size_t cur_pad = t - ps;
    size_t new_pad;
    if (b->len <= target_len) {
        new_pad = cur_pad + (target_len - b->len);
    } else if (b->len - target_len <= cur_pad) {
        new_pad = cur_pad - (b->len - target_len);
    } else {
        new_pad = 2048;
    }
    if (new_pad == cur_pad) return;
    char *pad = (char *)malloc(new_pad ? new_pad : 1);
    if (!pad) return; // padding is cosmetic; keep the edits
    for (size_t i = 0; i < new_pad; i++) pad[i] = (i % 64 == 0) ? '\n' : ' ';
    xb_splice(b, ps, t, pad, new_pad);
    free(pad);
}

// Locate the catalog /Metadata stream; *out_num is its object number (0 when
// the stream is inlined in the catalog, which conforming files never do).
static TspdfObj *find_metadata_stream(TspdfReader *doc, TspdfParser *parser,
                                      uint32_t *out_num) {
    TspdfObj *catalog = doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT
        ? doc->catalog
        : (doc->xref.trailer
               ? tspdf_doctree_resolve(doc, parser,
                                       tspdf_dict_get(doc->xref.trailer, "Root"))
               : NULL);
    if (!catalog || catalog->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *ref = tspdf_dict_get(catalog, "Metadata");
    if (!ref) return NULL;
    *out_num = ref->type == TSPDF_OBJ_REF ? ref->ref.num : 0;
    TspdfObj *meta = tspdf_doctree_resolve(doc, parser, ref);
    if (!meta || meta->type != TSPDF_OBJ_STREAM) return NULL;
    return meta;
}

// Decode the packet bytes: decrypt when the document (and its metadata) is
// encrypted, then run the /Filter chain. Returns malloc'd bytes or NULL.
static uint8_t *metadata_decode(TspdfReader *doc, TspdfObj *meta, uint32_t num,
                                size_t *out_len) {
    TspdfObj *dict = meta->stream.dict;
    uint8_t *decoded = NULL;
    size_t dlen = 0;

    if (meta->stream.self_contained && meta->stream.data) {
        if (tspdf_stream_decode(dict, meta->stream.data, meta->stream.len,
                                &decoded, &dlen) != TSPDF_OK) {
            return NULL;
        }
        *out_len = dlen;
        return decoded;
    }

    if (!doc->data || meta->stream.raw_offset > doc->data_len ||
        meta->stream.raw_len > doc->data_len - meta->stream.raw_offset) {
        return NULL;
    }
    const uint8_t *raw = doc->data + meta->stream.raw_offset;
    size_t raw_len = meta->stream.raw_len;

    uint8_t *plain = NULL;
    if (doc->crypt && doc->crypt->encrypt_metadata && num > 0 && raw_len > 0) {
        uint16_t gen = num < doc->xref.count ? doc->xref.entries[num].gen : 0;
        size_t plen = 0;
        plain = tspdf_crypt_decrypt_stream(doc->crypt, num, gen, raw, raw_len,
                                           &plen);
        if (!plain) return NULL;
        raw = plain;
        raw_len = plen;
    }

    TspdfError err = tspdf_stream_decode(dict, raw, raw_len, &decoded, &dlen);
    free(plain);
    if (err != TSPDF_OK) return NULL;
    *out_len = dlen;
    return decoded;
}

static void dict_remove_key(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            memmove(&dict->dict.entries[i], &dict->dict.entries[i + 1],
                    (dict->dict.count - i - 1) * sizeof(TspdfDictEntry));
            dict->dict.count--;
            i--;
        }
    }
}

unsigned tspdf_reader_sync_xmp_metadata(TspdfReader *doc) {
    if (!doc || !doc->metadata) return 0;
    TspdfReaderMetadata *m = doc->metadata;

    const struct {
        unsigned bit;
        bool set;
        const char *value;
        const XmpProp *prop;
    } fields[] = {
        {TSPDF_XMP_TITLE, m->title_set, m->title, &PROP_TITLE},
        {TSPDF_XMP_AUTHOR, m->author_set, m->author, &PROP_AUTHOR},
        {TSPDF_XMP_SUBJECT, m->subject_set, m->subject, &PROP_SUBJECT},
        {TSPDF_XMP_KEYWORDS, m->keywords_set, m->keywords, &PROP_KEYWORDS},
        {TSPDF_XMP_CREATOR, m->creator_set, m->creator, &PROP_CREATOR},
        {TSPDF_XMP_PRODUCER, m->producer_set, m->producer, &PROP_PRODUCER},
    };
    const size_t nfields = sizeof(fields) / sizeof(fields[0]);

    unsigned requested = 0;
    for (size_t i = 0; i < nfields; i++) {
        if (fields[i].set) requested |= fields[i].bit;
    }
    if (requested == 0) return 0;

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    uint32_t num = 0;
    TspdfObj *meta = find_metadata_stream(doc, &parser, &num);
    if (!meta) return 0; // no XMP: nothing can go stale
    if (doc->crypt && num == 0) return requested;

    size_t orig_len = 0;
    uint8_t *packet = metadata_decode(doc, meta, num, &orig_len);
    if (!packet) return requested;
    if (!xmp_packet_editable(packet, orig_len)) {
        free(packet);
        return requested;
    }

    XBuf b = {packet, orig_len, orig_len};
    unsigned stale = 0;
    unsigned synced = 0;
    for (size_t i = 0; i < nfields; i++) {
        if (!fields[i].set) continue;
        if (xmp_apply_field(&b, fields[i].prop, fields[i].value)) {
            synced |= fields[i].bit;
        } else {
            stale |= fields[i].bit;
        }
    }
    if (synced == 0) {
        free(b.data);
        return stale;
    }

    // The packet changes, so the save will stamp a fresh Info /ModDate; put
    // the very same timestamp into xmp:ModifyDate (best effort — a packet
    // without the property keeps none) and hand the D: form to the writer.
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    if (tm_info) {
        char iso[40];
        char pdfd[40];
        snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        snprintf(pdfd, sizeof(pdfd), "D:%04d%02d%02d%02d%02d%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        xmp_apply_field(&b, &PROP_MODIFY_DATE, iso);
        free(m->mod_date);
        m->mod_date = strdup(pdfd);
    }

    xmp_fix_padding(&b, orig_len);

    // Install the edited packet as the stream's bytes. It is stored
    // uncompressed (XMP scanners expect that), so the filter entries go.
    dict_remove_key(meta->stream.dict, "Filter");
    dict_remove_key(meta->stream.dict, "DecodeParms");
    dict_remove_key(meta->stream.dict, "DP");
    TspdfObj *len_obj = tspdf_dict_get(meta->stream.dict, "Length");
    if (len_obj && len_obj->type == TSPDF_OBJ_INT) {
        len_obj->integer = (int64_t)b.len;
    }
    free(meta->stream.data);
    meta->stream.data = b.data;
    meta->stream.len = b.len;
    meta->stream.self_contained = true;
    doc->modified = true;
    return stale;
}
