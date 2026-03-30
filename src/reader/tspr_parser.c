#include "tspr_internal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// --- Helpers ---

static inline bool at_end(TspdfParser *p) {
    return p->pos >= p->len;
}

static inline uint8_t peek(TspdfParser *p) {
    if (at_end(p)) return 0;
    return p->data[p->pos];
}

static inline uint8_t advance(TspdfParser *p) {
    if (at_end(p)) return 0;
    return p->data[p->pos++];
}

static inline bool match(TspdfParser *p, const char *s) {
    size_t slen = strlen(s);
    if (p->pos + slen > p->len) return false;
    if (memcmp(p->data + p->pos, s, slen) == 0) {
        p->pos += slen;
        return true;
    }
    return false;
}

static inline bool is_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0' || c == '\f';
}

static inline bool is_delim(uint8_t c) {
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static inline bool is_ws_or_delim(uint8_t c) {
    return is_ws(c) || is_delim(c);
}

static int hex_val(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static TspdfObj *alloc_obj(TspdfParser *p) {
    TspdfObj *obj = (TspdfObj *)tspdf_arena_alloc_zero(p->arena, sizeof(TspdfObj));
    if (!obj) {
        p->error = TSPDF_ERR_ALLOC;
    }
    return obj;
}

// --- Skip whitespace and comments ---

void tspdf_skip_whitespace(TspdfParser *p) {
    while (!at_end(p)) {
        uint8_t c = peek(p);
        if (is_ws(c)) {
            p->pos++;
        } else if (c == '%') {
            // Skip comment until end of line
            while (!at_end(p) && peek(p) != '\n' && peek(p) != '\r') {
                p->pos++;
            }
        } else {
            break;
        }
    }
}

// --- Parse literal string ---

static TspdfObj *tspdf_parse_literal_string(TspdfParser *p) {
    p->pos++; // skip '('
    // First pass: compute length. Second pass: fill buffer.
    // For simplicity, use a temp buffer up to remaining size.
    size_t max_len = p->len - p->pos;
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(p->arena, max_len + 1);
    if (!buf) { p->error = TSPDF_ERR_ALLOC; return NULL; }

    size_t out = 0;
    int depth = 1;

    while (!at_end(p) && depth > 0) {
        uint8_t c = advance(p);
        if (c == '(') {
            depth++;
            buf[out++] = c;
        } else if (c == ')') {
            depth--;
            if (depth > 0) buf[out++] = c;
        } else if (c == '\\') {
            if (at_end(p)) break;
            uint8_t next = advance(p);
            switch (next) {
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 'f':  buf[out++] = '\f'; break;
                case '\\': buf[out++] = '\\'; break;
                case '(':  buf[out++] = '(';  break;
                case ')':  buf[out++] = ')';  break;
                case '\r':
                    // line continuation: skip \r and optional \n
                    if (!at_end(p) && peek(p) == '\n') p->pos++;
                    break;
                case '\n':
                    // line continuation
                    break;
                default:
                    // Octal escape
                    if (next >= '0' && next <= '7') {
                        int val = next - '0';
                        if (!at_end(p) && peek(p) >= '0' && peek(p) <= '7') {
                            val = val * 8 + (advance(p) - '0');
                            if (!at_end(p) && peek(p) >= '0' && peek(p) <= '7') {
                                val = val * 8 + (advance(p) - '0');
                            }
                        }
                        buf[out++] = (uint8_t)val;
                    } else {
                        buf[out++] = next;
                    }
                    break;
            }
        } else {
            buf[out++] = c;
        }
    }

    if (depth != 0) {
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    buf[out] = '\0';

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_STRING;
    obj->string.data = buf;
    obj->string.len = out;
    return obj;
}

// --- Parse hex string ---

static TspdfObj *tspdf_parse_hex_string(TspdfParser *p) {
    p->pos++; // skip '<'

    // Count hex digits first to size the buffer
    size_t max_bytes = (p->len - p->pos) / 2 + 1;
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(p->arena, max_bytes + 1);
    if (!buf) { p->error = TSPDF_ERR_ALLOC; return NULL; }

    size_t out = 0;
    int nibble = -1; // -1 means no pending nibble

    while (!at_end(p)) {
        uint8_t c = advance(p);
        if (c == '>') break;
        if (is_ws(c)) continue;
        int v = hex_val(c);
        if (v < 0) {
            p->error = TSPDF_ERR_PARSE;
            return NULL;
        }
        if (nibble < 0) {
            nibble = v;
        } else {
            buf[out++] = (uint8_t)((nibble << 4) | v);
            nibble = -1;
        }
    }

    // Trailing odd nibble gets implicit 0
    if (nibble >= 0) {
        buf[out++] = (uint8_t)(nibble << 4);
    }

    buf[out] = '\0';

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_STRING;
    obj->string.data = buf;
    obj->string.len = out;
    return obj;
}

// --- Parse name ---

static TspdfObj *tspdf_parse_name(TspdfParser *p) {
    p->pos++; // skip '/'

    size_t start = p->pos;
    // Find end of name
    while (!at_end(p) && !is_ws_or_delim(peek(p))) {
        p->pos++;
    }
    size_t raw_len = p->pos - start;

    // Decode #XX escapes
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(p->arena, raw_len + 1);
    if (!buf) { p->error = TSPDF_ERR_ALLOC; return NULL; }

    size_t out = 0;
    for (size_t i = 0; i < raw_len; i++) {
        uint8_t c = p->data[start + i];
        if (c == '#' && i + 2 < raw_len) {
            int hi = hex_val(p->data[start + i + 1]);
            int lo = hex_val(p->data[start + i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[out++] = (uint8_t)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[out++] = c;
    }
    buf[out] = '\0';

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_NAME;
    obj->string.data = buf;
    obj->string.len = out;
    return obj;
}

// --- Parse array ---

static TspdfObj *tspdf_parse_array(TspdfParser *p) {
    p->pos++; // skip '['

    // Parse into a temporary list, then copy to arena
    size_t cap = 16;
    size_t count = 0;
    TspdfObj *items = (TspdfObj *)tspdf_arena_alloc(p->arena, cap * sizeof(TspdfObj));
    if (!items) { p->error = TSPDF_ERR_ALLOC; return NULL; }

    while (1) {
        tspdf_skip_whitespace(p);
        if (at_end(p)) { p->error = TSPDF_ERR_PARSE; return NULL; }
        if (peek(p) == ']') { p->pos++; break; }

        TspdfObj *elem = tspdf_parse_obj(p);
        if (!elem) return NULL;

        if (count >= cap) {
            // Grow: allocate new array in arena
            size_t new_cap = cap * 2;
            TspdfObj *new_items = (TspdfObj *)tspdf_arena_alloc(p->arena, new_cap * sizeof(TspdfObj));
            if (!new_items) { p->error = TSPDF_ERR_ALLOC; return NULL; }
            memcpy(new_items, items, count * sizeof(TspdfObj));
            items = new_items;
            cap = new_cap;
        }
        items[count++] = *elem;
    }

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_ARRAY;
    obj->array.items = items;
    obj->array.count = count;
    return obj;
}

// --- Parse dict ---

static TspdfObj *tspdf_parse_dict(TspdfParser *p) {
    // '<<' already consumed by caller

    size_t cap = 16;
    size_t count = 0;
    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(p->arena, cap * sizeof(TspdfDictEntry));
    if (!entries) { p->error = TSPDF_ERR_ALLOC; return NULL; }

    while (1) {
        tspdf_skip_whitespace(p);
        if (at_end(p)) { p->error = TSPDF_ERR_PARSE; return NULL; }

        // Check for '>>'
        if (peek(p) == '>') {
            if (p->pos + 1 < p->len && p->data[p->pos + 1] == '>') {
                p->pos += 2;
                break;
            }
        }

        // Key must be a name
        if (peek(p) != '/') {
            p->error = TSPDF_ERR_PARSE;
            return NULL;
        }
        TspdfObj *key_obj = tspdf_parse_name(p);
        if (!key_obj) return NULL;

        tspdf_skip_whitespace(p);

        TspdfObj *val = tspdf_parse_obj(p);
        if (!val) return NULL;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            TspdfDictEntry *new_entries = (TspdfDictEntry *)tspdf_arena_alloc(p->arena, new_cap * sizeof(TspdfDictEntry));
            if (!new_entries) { p->error = TSPDF_ERR_ALLOC; return NULL; }
            memcpy(new_entries, entries, count * sizeof(TspdfDictEntry));
            entries = new_entries;
            cap = new_cap;
        }

        entries[count].key = (char *)key_obj->string.data;
        entries[count].value = val;
        count++;
    }

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_DICT;
    obj->dict.entries = entries;
    obj->dict.count = count;
    return obj;
}

// --- Parse number and possible indirect reference ---

static TspdfObj *tspdf_parse_number(TspdfParser *p) {
    size_t start = p->pos;

    // Consume sign
    if (peek(p) == '+' || peek(p) == '-') p->pos++;

    bool has_dot = false;
    while (!at_end(p) && (isdigit(peek(p)) || peek(p) == '.')) {
        if (peek(p) == '.') {
            if (has_dot) break; // second dot, stop
            has_dot = true;
        }
        p->pos++;
    }

    size_t num_end = p->pos;
    if (num_end == start) {
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    // Extract the number string
    char numbuf[64];
    size_t nlen = num_end - start;
    if (nlen >= sizeof(numbuf)) nlen = sizeof(numbuf) - 1;
    memcpy(numbuf, p->data + start, nlen);
    numbuf[nlen] = '\0';

    if (has_dot) {
        TspdfObj *obj = alloc_obj(p);
        if (!obj) return NULL;
        obj->type = TSPDF_OBJ_REAL;
        obj->real = strtod(numbuf, NULL);
        return obj;
    }

    int64_t int_val = strtoll(numbuf, NULL, 10);

    // Look ahead for "G R" pattern (indirect reference) or "G obj" (indirect object)
    // Only if the number is non-negative (object numbers are non-negative)
    if (int_val >= 0 && p->data[start] != '+') {
        size_t saved = p->pos;
        tspdf_skip_whitespace(p);

        // Try to read second number (generation)
        if (!at_end(p) && isdigit(peek(p))) {
            size_t gen_start = p->pos;
            while (!at_end(p) && isdigit(peek(p))) p->pos++;
            size_t gen_end = p->pos;

            char genbuf[32];
            size_t glen = gen_end - gen_start;
            if (glen < sizeof(genbuf)) {
                memcpy(genbuf, p->data + gen_start, glen);
                genbuf[glen] = '\0';
                int64_t gen_val = strtoll(genbuf, NULL, 10);

                tspdf_skip_whitespace(p);

                if (!at_end(p) && peek(p) == 'R') {
                    p->pos++; // consume 'R'
                    TspdfObj *obj = alloc_obj(p);
                    if (!obj) return NULL;
                    obj->type = TSPDF_OBJ_REF;
                    obj->ref.num = (uint32_t)int_val;
                    obj->ref.gen = (uint16_t)gen_val;
                    return obj;
                }
            }
        }

        // Not a reference, restore position
        p->pos = saved;
    }

    TspdfObj *obj = alloc_obj(p);
    if (!obj) return NULL;
    obj->type = TSPDF_OBJ_INT;
    obj->integer = int_val;
    return obj;
}

// --- Main parse dispatch ---

void tspdf_parser_init(TspdfParser *p, const uint8_t *data, size_t len, TspdfArena *arena) {
    p->data = data;
    p->len = len;
    p->pos = 0;
    p->arena = arena;
    p->error = TSPDF_OK;
}

TspdfObj *tspdf_parse_obj(TspdfParser *p) {
    tspdf_skip_whitespace(p);
    if (at_end(p)) {
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    uint8_t c = peek(p);

    if (c == '(') return tspdf_parse_literal_string(p);

    if (c == '<') {
        if (p->pos + 1 < p->len && p->data[p->pos + 1] == '<') {
            p->pos += 2; // consume '<<'
            return tspdf_parse_dict(p);
        }
        return tspdf_parse_hex_string(p);
    }

    if (c == '[') return tspdf_parse_array(p);
    if (c == '/') return tspdf_parse_name(p);

    if (c == 't') {
        if (match(p, "true")) {
            TspdfObj *obj = alloc_obj(p);
            if (!obj) return NULL;
            obj->type = TSPDF_OBJ_BOOL;
            obj->boolean = true;
            return obj;
        }
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    if (c == 'f') {
        if (match(p, "false")) {
            TspdfObj *obj = alloc_obj(p);
            if (!obj) return NULL;
            obj->type = TSPDF_OBJ_BOOL;
            obj->boolean = false;
            return obj;
        }
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    if (c == 'n') {
        if (match(p, "null")) {
            TspdfObj *obj = alloc_obj(p);
            if (!obj) return NULL;
            obj->type = TSPDF_OBJ_NULL;
            return obj;
        }
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    if (isdigit(c) || c == '+' || c == '-' || c == '.') {
        return tspdf_parse_number(p);
    }

    p->error = TSPDF_ERR_PARSE;
    return NULL;
}

// --- Parse indirect object definition ---

TspdfObj *tspdf_parse_indirect_obj(TspdfParser *p, uint32_t *out_num, uint16_t *out_gen) {
    tspdf_skip_whitespace(p);

    // Parse object number
    size_t start = p->pos;
    while (!at_end(p) && isdigit(peek(p))) p->pos++;
    if (p->pos == start) { p->error = TSPDF_ERR_PARSE; return NULL; }
    char numbuf[32];
    size_t nlen = p->pos - start;
    if (nlen >= sizeof(numbuf)) { p->error = TSPDF_ERR_PARSE; return NULL; }
    memcpy(numbuf, p->data + start, nlen);
    numbuf[nlen] = '\0';
    uint32_t num = (uint32_t)strtoul(numbuf, NULL, 10);

    tspdf_skip_whitespace(p);

    // Parse generation number
    start = p->pos;
    while (!at_end(p) && isdigit(peek(p))) p->pos++;
    if (p->pos == start) { p->error = TSPDF_ERR_PARSE; return NULL; }
    nlen = p->pos - start;
    if (nlen >= sizeof(numbuf)) { p->error = TSPDF_ERR_PARSE; return NULL; }
    memcpy(numbuf, p->data + start, nlen);
    numbuf[nlen] = '\0';
    uint16_t gen = (uint16_t)strtoul(numbuf, NULL, 10);

    tspdf_skip_whitespace(p);

    // Expect "obj"
    if (!match(p, "obj")) {
        p->error = TSPDF_ERR_PARSE;
        return NULL;
    }

    *out_num = num;
    *out_gen = gen;

    // Parse the inner object
    TspdfObj *inner = tspdf_parse_obj(p);
    if (!inner) return NULL;

    tspdf_skip_whitespace(p);

    // Check if this is a stream (dict followed by "stream" keyword)
    if (inner->type == TSPDF_OBJ_DICT && !at_end(p) && match(p, "stream")) {
        // Skip single newline after "stream": CR, LF, or CRLF
        if (!at_end(p)) {
            if (peek(p) == '\r') {
                p->pos++;
                if (!at_end(p) && peek(p) == '\n') p->pos++;
            } else if (peek(p) == '\n') {
                p->pos++;
            }
        }

        size_t raw_offset = p->pos;

        // Get /Length from dict
        TspdfObj *len_obj = tspdf_dict_get(inner, "Length");
        size_t raw_len = 0;
        if (len_obj && len_obj->type == TSPDF_OBJ_INT) {
            raw_len = (size_t)len_obj->integer;
        }

        // Advance past stream data
        size_t expected_end = raw_offset + raw_len;
        if (expected_end <= p->len) {
            p->pos = expected_end;
        }

        // Try to find "endstream"
        tspdf_skip_whitespace(p);
        if (!match(p, "endstream")) {
            // Fallback: scan forward for "endstream" from raw_offset
            const char *needle = "endstream";
            size_t needle_len = 9;
            bool found = false;
            for (size_t i = raw_offset; i + needle_len <= p->len; i++) {
                if (memcmp(p->data + i, needle, needle_len) == 0) {
                    raw_len = i - raw_offset;
                    // Trim trailing whitespace from raw data
                    while (raw_len > 0 && is_ws(p->data[raw_offset + raw_len - 1])) {
                        raw_len--;
                    }
                    p->pos = i + needle_len;
                    found = true;
                    break;
                }
            }
            if (!found) {
                p->error = TSPDF_ERR_PARSE;
                return NULL;
            }
        }

        TspdfObj *stream_obj = alloc_obj(p);
        if (!stream_obj) return NULL;
        stream_obj->type = TSPDF_OBJ_STREAM;
        stream_obj->stream.dict = inner;
        stream_obj->stream.data = NULL;
        stream_obj->stream.len = 0;
        stream_obj->stream.raw_offset = raw_offset;
        stream_obj->stream.raw_len = raw_len;

        tspdf_skip_whitespace(p);
        match(p, "endobj"); // consume endobj

        return stream_obj;
    }

    // Expect "endobj"
    tspdf_skip_whitespace(p);
    if (!match(p, "endobj")) {
        // Not fatal for robustness, but note the error
        // Some PDFs may have issues here, keep going
    }

    return inner;
}
