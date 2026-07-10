#include "tspr_internal.h"
#include "../util/buffer.h"
#include "../util/pdftext.h"
#include "../compress/deflate.h"
#include "../../include/tspdf/version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

// --- Forward declarations for write helpers (defined later) ---
static void write_string_escaped(TspdfBuffer *buf, const uint8_t *data, size_t len);
static void write_hex_string(TspdfBuffer *buf, const uint8_t *data, size_t len);
static void write_name(TspdfBuffer *buf, const uint8_t *data, size_t len);
static bool is_xmp_metadata(TspdfObj *obj);
static bool stream_dict_type_is(TspdfObj *obj, const char *type_name);
static bool is_serialized_page_object(TspdfReader *doc, uint32_t obj_num, TspdfObj *obj);

// True when the source file stored any object inside an object stream. A
// rewrite of such a file re-packs eligible objects into fresh ObjStms
// (preserve-style, qpdf's default): exploding them into classic top-level
// objects roughly doubles ObjStm-heavy files. Classic inputs are never
// force-packed.
static bool input_uses_objstm(const TspdfReader *doc) {
    for (size_t i = 1; i < doc->xref.count; i++) {
        if (doc->xref.entries[i].in_use && doc->xref.entries[i].compressed) {
            return true;
        }
    }
    return false;
}

// ObjStm and XRef stream objects are cross-reference machinery, not document
// content: nothing in the object graph may reference them, and a full rewrite
// re-derives both (members are written individually or re-packed, and a fresh
// xref is emitted). Copying the source containers would duplicate every
// packed object, so every save path drops them.
static bool is_xref_machinery(TspdfObj *obj) {
    return stream_dict_type_is(obj, "ObjStm") || stream_dict_type_is(obj, "XRef");
}

// --- Metadata helpers ---

// Check if metadata has any set flags
static bool metadata_has_changes(const TspdfReaderMetadata *m) {
    if (!m) return false;
    return m->title_set || m->author_set || m->subject_set ||
           m->keywords_set || m->creator_set || m->producer_set;
}

// Write one Info-dict string value. In an encrypted file every string outside
// the /Encrypt dictionary and /ID must be encrypted (ISO 32000 §7.6.2) —
// Info strings included. Readers decrypt them unconditionally, so plaintext
// here turns into garbage (poppler showed blank titles).
static void write_info_string_bytes(TspdfBuffer *buf, TspdfCrypt *crypt,
                                    uint32_t info_obj_num,
                                    const uint8_t *data, size_t len) {
    if (crypt) {
        size_t enc_len = 0;
        uint8_t *enc = tspdf_crypt_encrypt_string(crypt, info_obj_num, 0,
                                                  data, len, &enc_len);
        if (enc) {
            write_hex_string(buf, enc, enc_len);
            free(enc);
            return;
        }
    }
    write_string_escaped(buf, data, len);
}

// UTF-8 metadata override values go through the shared text-string encoder
// (ASCII stays literal, everything else becomes BOM + UTF-16BE) and are then
// encrypted like any other string when the save is encrypted.
static void write_info_utf8_value(TspdfBuffer *buf, TspdfCrypt *crypt,
                                  uint32_t info_obj_num, TspdfArena *arena,
                                  const char *value) {
    if (!crypt) {
        tspdf_pdftext_write_info_string(buf, value);
        return;
    }
    size_t blen = 0;
    uint8_t *bytes = tspdf_utf8_to_pdf_text(value, &blen, arena);
    if (!bytes) {
        buf->error = true;
        return;
    }
    write_info_string_bytes(buf, crypt, info_obj_num, bytes, blen);
}

// Build and write an Info dict object at current buffer position.
// Merges original Info dict fields with overrides from doc->metadata.
// When `crypt` is non-NULL the object is being written into an encrypted
// file: every string value is encrypted with the Info object's own key.
static void write_info_dict_obj(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                                 uint32_t info_obj_num, TspdfCrypt *crypt) {
    // Resolve original Info dict if present. Resolve with the document's own
    // crypt so preserved values from an encrypted source arrive decrypted
    // (they are re-encrypted with the new object's key on write below).
    TspdfObj *orig_info = NULL;
    if (doc->xref.trailer) {
        TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
        if (info_ref) {
            if (info_ref->type == TSPDF_OBJ_REF) {
                uint32_t num = info_ref->ref.num;
                if (num < doc->xref.count) {
                    orig_info = tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache, doc->crypt);
                }
            } else if (info_ref->type == TSPDF_OBJ_DICT) {
                orig_info = info_ref;
            }
        }
    }

    TspdfReaderMetadata *m = doc->metadata;

    // Generate ModDate timestamp: D:YYYYMMDDHHmmSS
    char mod_date[64];
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    snprintf(mod_date, sizeof(mod_date), "D:%04d%02d%02d%02d%02d%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    // Field names and their metadata override values (NULL = not overridden, "" ptr = set to remove)
    static const char *field_names[] = {
        "Title", "Author", "Subject", "Keywords", "Creator", "Producer", NULL
    };

    tspdf_buffer_printf(buf, "%u 0 obj\n<< ", info_obj_num);

    // Write fields from original Info, applying overrides
    if (orig_info && orig_info->type == TSPDF_OBJ_DICT) {
        for (size_t i = 0; i < orig_info->dict.count; i++) {
            const char *key = orig_info->dict.entries[i].key;
            TspdfObj *val = orig_info->dict.entries[i].value;

            // Skip ModDate — we'll add our own
            if (strcmp(key, "ModDate") == 0) continue;

            // Check if this field is overridden
            const char *override_val = NULL;
            bool is_overridden = false;
            if (m) {
                if (strcmp(key, "Title") == 0 && m->title_set) {
                    override_val = m->title; is_overridden = true;
                } else if (strcmp(key, "Author") == 0 && m->author_set) {
                    override_val = m->author; is_overridden = true;
                } else if (strcmp(key, "Subject") == 0 && m->subject_set) {
                    override_val = m->subject; is_overridden = true;
                } else if (strcmp(key, "Keywords") == 0 && m->keywords_set) {
                    override_val = m->keywords; is_overridden = true;
                } else if (strcmp(key, "Creator") == 0 && m->creator_set) {
                    override_val = m->creator; is_overridden = true;
                } else if (strcmp(key, "Producer") == 0 && m->producer_set) {
                    override_val = m->producer; is_overridden = true;
                }
            }

            if (is_overridden) {
                // Write override value if non-NULL, otherwise skip (remove field)
                if (override_val != NULL) {
                    write_name(buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(buf, ' ');
                    write_info_utf8_value(buf, crypt, info_obj_num, parser->arena, override_val);
                    tspdf_buffer_append_byte(buf, ' ');
                }
            } else {
                // Keep original value (skip refs, copy simple values)
                if (val && val->type == TSPDF_OBJ_STRING) {
                    write_name(buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(buf, ' ');
                    write_info_string_bytes(buf, crypt, info_obj_num,
                                            val->string.data, val->string.len);
                    tspdf_buffer_append_byte(buf, ' ');
                }
                // Skip non-string values (unusual in Info dict)
            }
        }
    }

    // Now write new fields that weren't in the original Info dict
    if (m) {
        for (int fi = 0; field_names[fi] != NULL; fi++) {
            const char *fname = field_names[fi];

            // Check if this field was set in metadata
            const char *new_val = NULL;
            bool was_set = false;
            if (strcmp(fname, "Title") == 0 && m->title_set) {
                new_val = m->title; was_set = true;
            } else if (strcmp(fname, "Author") == 0 && m->author_set) {
                new_val = m->author; was_set = true;
            } else if (strcmp(fname, "Subject") == 0 && m->subject_set) {
                new_val = m->subject; was_set = true;
            } else if (strcmp(fname, "Keywords") == 0 && m->keywords_set) {
                new_val = m->keywords; was_set = true;
            } else if (strcmp(fname, "Creator") == 0 && m->creator_set) {
                new_val = m->creator; was_set = true;
            } else if (strcmp(fname, "Producer") == 0 && m->producer_set) {
                new_val = m->producer; was_set = true;
            }

            if (!was_set || new_val == NULL) continue;

            // Only write if not already present in original (already handled above)
            bool already_in_orig = false;
            if (orig_info && orig_info->type == TSPDF_OBJ_DICT) {
                for (size_t i = 0; i < orig_info->dict.count; i++) {
                    if (strcmp(orig_info->dict.entries[i].key, fname) == 0) {
                        already_in_orig = true;
                        break;
                    }
                }
            }
            if (already_in_orig) continue;

            write_name(buf, (const uint8_t *)fname, strlen(fname));
            tspdf_buffer_append_byte(buf, ' ');
            write_info_utf8_value(buf, crypt, info_obj_num, parser->arena, new_val);
            tspdf_buffer_append_byte(buf, ' ');
        }
    }

    // Add /CreationDate if not already present
    bool has_creation_date = false;
    if (orig_info && orig_info->type == TSPDF_OBJ_DICT) {
        for (size_t i = 0; i < orig_info->dict.count; i++) {
            if (strcmp(orig_info->dict.entries[i].key, "CreationDate") == 0) {
                has_creation_date = true;
                break;
            }
        }
    }
    if (!has_creation_date) {
        write_name(buf, (const uint8_t *)"CreationDate", 12);
        tspdf_buffer_append_byte(buf, ' ');
        write_info_string_bytes(buf, crypt, info_obj_num,
                                (const uint8_t *)mod_date, strlen(mod_date));
        tspdf_buffer_append_byte(buf, ' ');
    }

    // Always add /ModDate
    write_name(buf, (const uint8_t *)"ModDate", 7);
    tspdf_buffer_append_byte(buf, ' ');
    write_info_string_bytes(buf, crypt, info_obj_num,
                            (const uint8_t *)mod_date, strlen(mod_date));
    tspdf_buffer_append_str(buf, " >>\nendobj\n");
}

// --- Object collection: find all objects reachable from pages ---

static void collect_from_obj(TspdfReader *doc, TspdfObj *obj, bool *visited, TspdfParser *parser);

static TspdfObj *resolve_for_collect(TspdfReader *doc, uint32_t num, TspdfParser *parser) {
    if (num < doc->xref.count) {
        return tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache, doc->crypt);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
}

static TspdfObj *catalog_for_serialize(TspdfReader *doc, TspdfParser *parser) {
    if (doc && doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT) {
        return doc->catalog;
    }

    if (!doc || !doc->xref.trailer) {
        return NULL;
    }

    TspdfObj *root = tspdf_dict_get(doc->xref.trailer, "Root");
    if (!root) {
        return NULL;
    }
    if (root->type == TSPDF_OBJ_DICT) {
        return root;
    }
    if (root->type != TSPDF_OBJ_REF || root->ref.num >= doc->xref.count) {
        return NULL;
    }

    TspdfObj *catalog = tspdf_xref_resolve(&doc->xref, parser, root->ref.num,
                                           doc->obj_cache, NULL);
    return catalog && catalog->type == TSPDF_OBJ_DICT ? catalog : NULL;
}

static bool preserve_catalog_key(const char *key, bool strip_metadata) {
    if (!key || strcmp(key, "Pages") == 0) {
        return false;
    }
    if (strip_metadata && strcmp(key, "Metadata") == 0) {
        return false;
    }
    return true;
}

static void collect_from_ref(TspdfReader *doc, uint32_t obj_num, bool *visited, TspdfParser *parser) {
    if (obj_num >= doc->xref.count + doc->new_objs.count) return;
    if (visited[obj_num]) return;
    visited[obj_num] = true;

    TspdfObj *resolved = resolve_for_collect(doc, obj_num, parser);
    if (!resolved) return;

    collect_from_obj(doc, resolved, visited, parser);
}

static void collect_from_obj(TspdfReader *doc, TspdfObj *obj, bool *visited, TspdfParser *parser) {
    if (!obj) return;

    switch (obj->type) {
        case TSPDF_OBJ_REF:
            collect_from_ref(doc, obj->ref.num, visited, parser);
            break;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                collect_from_obj(doc, &obj->array.items[i], visited, parser);
            }
            break;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                collect_from_obj(doc, obj->dict.entries[i].value, visited, parser);
            }
            break;
        case TSPDF_OBJ_STREAM:
            collect_from_obj(doc, obj->stream.dict, visited, parser);
            break;
        default:
            break;
    }
}

static void collect_from_catalog_entries(TspdfReader *doc, TspdfObj *catalog,
                                         bool *visited, TspdfParser *parser,
                                         bool strip_metadata) {
    if (!catalog || catalog->type != TSPDF_OBJ_DICT) {
        return;
    }

    for (size_t i = 0; i < catalog->dict.count; i++) {
        const char *key = catalog->dict.entries[i].key;
        if (!preserve_catalog_key(key, strip_metadata)) {
            continue;
        }
        collect_from_obj(doc, catalog->dict.entries[i].value, visited, parser);
    }
}

static void collect_from_page_obj(TspdfReader *doc, TspdfObj *page_dict,
                                  bool *visited, TspdfParser *parser) {
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) {
        collect_from_obj(doc, page_dict, visited, parser);
        return;
    }

    for (size_t i = 0; i < page_dict->dict.count; i++) {
        if (strcmp(page_dict->dict.entries[i].key, "Parent") == 0) {
            continue;
        }
        collect_from_obj(doc, page_dict->dict.entries[i].value, visited, parser);
    }
}

// --- Object writing ---

typedef struct {
    uint32_t *old_to_new;  // old_to_new[old_num] = new_num, 0 means not mapped
    size_t map_size;
} RenumberMap;

static void write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc);

static void write_string_escaped(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(buf, '(');
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (c) {
            case '(':  tspdf_buffer_append_str(buf, "\\("); break;
            case ')':  tspdf_buffer_append_str(buf, "\\)"); break;
            case '\\': tspdf_buffer_append_str(buf, "\\\\"); break;
            case '\n': tspdf_buffer_append_str(buf, "\\n"); break;
            case '\r': tspdf_buffer_append_str(buf, "\\r"); break;
            case '\t': tspdf_buffer_append_str(buf, "\\t"); break;
            case '\b': tspdf_buffer_append_str(buf, "\\b"); break;
            case '\f': tspdf_buffer_append_str(buf, "\\f"); break;
            default:
                if (c < 32 || c > 126) {
                    tspdf_buffer_printf(buf, "\\%03o", c);
                } else {
                    tspdf_buffer_append_byte(buf, c);
                }
                break;
        }
    }
    tspdf_buffer_append_byte(buf, ')');
}

static void write_name(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(buf, '/');
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        // Escape non-regular characters per PDF spec
        if (c < 33 || c > 126 || c == '#' || c == '/' || c == '(' || c == ')' ||
            c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
            c == '%') {
            tspdf_buffer_printf(buf, "#%02X", c);
        } else {
            tspdf_buffer_append_byte(buf, c);
        }
    }
}

static void write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc) {
    if (!obj) {
        tspdf_buffer_append_str(buf, "null");
        return;
    }

    switch (obj->type) {
        case TSPDF_OBJ_NULL:
            tspdf_buffer_append_str(buf, "null");
            break;
        case TSPDF_OBJ_BOOL:
            tspdf_buffer_append_str(buf, obj->boolean ? "true" : "false");
            break;
        case TSPDF_OBJ_INT:
            tspdf_buffer_printf(buf, "%lld", (long long)obj->integer);
            break;
        case TSPDF_OBJ_REAL: {
            // Use enough precision but strip trailing zeros
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.6f", obj->real);
            // Strip trailing zeros after decimal point
            char *dot = strchr(tmp, '.');
            if (dot) {
                char *end = tmp + strlen(tmp) - 1;
                while (end > dot && *end == '0') end--;
                if (end == dot) end--; // remove the dot too if no decimals left
                end[1] = '\0';
            }
            tspdf_buffer_append_str(buf, tmp);
            break;
        }
        case TSPDF_OBJ_STRING:
            write_string_escaped(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_NAME:
            write_name(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            tspdf_buffer_append_str(buf, "[ ");
            for (size_t i = 0; i < obj->array.count; i++) {
                if (i > 0) tspdf_buffer_append_byte(buf, ' ');
                write_obj(buf, &obj->array.items[i], map, doc);
            }
            tspdf_buffer_append_str(buf, " ]");
            break;
        case TSPDF_OBJ_DICT:
            tspdf_buffer_append_str(buf, "<< ");
            for (size_t i = 0; i < obj->dict.count; i++) {
                write_name(buf, (const uint8_t *)obj->dict.entries[i].key,
                           strlen(obj->dict.entries[i].key));
                tspdf_buffer_append_byte(buf, ' ');
                write_obj(buf, obj->dict.entries[i].value, map, doc);
                tspdf_buffer_append_byte(buf, ' ');
            }
            tspdf_buffer_append_str(buf, ">>");
            break;
        case TSPDF_OBJ_STREAM:
            // Should not be reached directly - streams are handled at top level
            break;
        case TSPDF_OBJ_REF: {
            uint32_t new_num = 0;
            if (obj->ref.num < map->map_size) {
                new_num = map->old_to_new[obj->ref.num];
            }
            if (new_num == 0) {
                // Object not in our set - write null reference
                tspdf_buffer_append_str(buf, "null");
            } else {
                tspdf_buffer_printf(buf, "%u 0 R", new_num);
            }
            break;
        }
    }
}

// Serialize a non-stream object body (no "N 0 obj" wrapper) with plain,
// unencrypted strings: page dicts get /Parent forced to the synthetic pages
// object (2 0 R) and /Type /Page ensured; everything else is written
// verbatim through the renumber map. Shared by the plain path and by the
// encrypted path's ObjStm packing (members of an encrypted object stream
// must NOT have individually encrypted strings — the container stream is
// encrypted as one unit).
static void write_nonstream_body_plain(TspdfBuffer *scratch, TspdfReader *doc,
                                       TspdfObj *obj, uint32_t old_num,
                                       const RenumberMap *map) {
    if (obj->type == TSPDF_OBJ_DICT && is_serialized_page_object(doc, old_num, obj)) {
        bool wrote_type = false;
        bool wrote_parent = false;
        tspdf_buffer_append_str(scratch, "<< ");
        for (size_t j = 0; j < obj->dict.count; j++) {
            const char *key = obj->dict.entries[j].key;
            write_name(scratch, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(scratch, ' ');
            if (strcmp(key, "Type") == 0) {
                wrote_type = true;
                write_obj(scratch, obj->dict.entries[j].value, map, doc);
            } else if (strcmp(key, "Parent") == 0) {
                wrote_parent = true;
                tspdf_buffer_append_str(scratch, "2 0 R");
            } else {
                write_obj(scratch, obj->dict.entries[j].value, map, doc);
            }
            tspdf_buffer_append_byte(scratch, ' ');
        }
        if (!wrote_type) {
            tspdf_buffer_append_str(scratch, "/Type /Page ");
        }
        if (!wrote_parent) {
            tspdf_buffer_append_str(scratch, "/Parent 2 0 R ");
        }
        tspdf_buffer_append_str(scratch, ">>");
    } else {
        write_obj(scratch, obj, map, doc);
    }
}

// Writes the catalog dict body only ("<< ... >>", no obj wrapper) so the
// caller can place it either top-level or inside an object stream.
static void write_catalog_body(TspdfBuffer *buf, TspdfObj *catalog,
                               const RenumberMap *map, TspdfReader *doc,
                               bool strip_metadata) {
    bool wrote_type = false;

    tspdf_buffer_append_str(buf, "<< ");
    if (catalog && catalog->type == TSPDF_OBJ_DICT) {
        for (size_t i = 0; i < catalog->dict.count; i++) {
            const char *key = catalog->dict.entries[i].key;
            if (!preserve_catalog_key(key, strip_metadata)) {
                continue;
            }

            if (strcmp(key, "Type") == 0) {
                wrote_type = true;
            }
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            write_obj(buf, catalog->dict.entries[i].value, map, doc);
            tspdf_buffer_append_byte(buf, ' ');
        }
    }

    if (!wrote_type) {
        tspdf_buffer_append_str(buf, "/Type /Catalog ");
    }
    tspdf_buffer_append_str(buf, "/Pages 2 0 R >>");
}

TspdfError tspdf_serialize(TspdfReader *doc, uint8_t **out_buf, size_t *out_len) {
    TspdfSaveOptions opts = tspdf_save_options_default();
    return tspdf_serialize_with_options(doc, out_buf, out_len, &opts);
}

// --- Helper: find end of an indirect object in raw PDF data ---
// Scans for "\nendobj" or "\rendobj" starting from 'start'.
// Returns position just past "endobj".
static size_t find_endobj(const uint8_t *data, size_t len, size_t start) {
    const uint8_t *needle = (const uint8_t *)"endobj";
    for (size_t i = start; i + 6 <= len; i++) {
        if (memcmp(data + i, needle, 6) == 0) {
            return i + 6;
        }
    }
    return len;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    uint8_t *owned;
} StreamBytes;

static bool stream_dict_type_is(TspdfObj *obj, const char *type_name) {
    if (!obj || obj->type != TSPDF_OBJ_STREAM || !type_name) {
        return false;
    }
    TspdfObj *type_val = tspdf_dict_get(obj->stream.dict, "Type");
    return type_val && type_val->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)type_val->string.data, type_name) == 0;
}

// Image XObjects get the fast deflate level during recompression: their
// Flate payloads (PNG-style, usually predictor-coded) gain almost nothing
// from the slow best-effort search, and scan-heavy files would otherwise
// dominate `tspdf compress` wall time for no size benefit.
static bool stream_is_image(TspdfObj *dict) {
    TspdfObj *st = tspdf_dict_get(dict, "Subtype");
    return st && st->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)st->string.data, "Image") == 0;
}

// True when the /Filter chain consists only of lossless byte filters our
// decoder can round-trip AND rewriting it as a bare /FlateDecode can win:
// either it contains a non-Flate "armor" stage (ASCIIHex/ASCII85/RunLength/
// LZW — those cost 25%+, hex doubles) or it is an array form like
// [/FlateDecode] (common in generator output; re-encoding normalizes it and
// applies the best-effort level). A bare FlateDecode NAME (with or without
// predictor params) is handled by the cheaper re-flate branch instead — the
// predictor usually helps compression, so full-decode-and-replace would
// only burn time to then keep the original.
static bool filter_chain_is_strippable(TspdfObj *filter) {
    if (!filter) return false;
    size_t count = filter->type == TSPDF_OBJ_ARRAY ? filter->array.count : 1;
    if (filter->type != TSPDF_OBJ_NAME && filter->type != TSPDF_OBJ_ARRAY) return false;
    bool has_armor = false;
    for (size_t i = 0; i < count; i++) {
        TspdfObj *name = filter->type == TSPDF_OBJ_NAME ? filter : &filter->array.items[i];
        if (!name || name->type != TSPDF_OBJ_NAME) return false;
        const char *f = (const char *)name->string.data;
        if (strcmp(f, "FlateDecode") == 0 || strcmp(f, "Fl") == 0) {
            continue;
        }
        if (strcmp(f, "ASCIIHexDecode") == 0 || strcmp(f, "AHx") == 0 ||
            strcmp(f, "ASCII85Decode") == 0 || strcmp(f, "A85") == 0 ||
            strcmp(f, "RunLengthDecode") == 0 || strcmp(f, "RL") == 0 ||
            strcmp(f, "LZWDecode") == 0 || strcmp(f, "LZW") == 0) {
            has_armor = true;
            continue;
        }
        return false;  // lossy or unknown filter: leave the stream verbatim
    }
    return has_armor || filter->type == TSPDF_OBJ_ARRAY;
}

// True when the stream's /DecodeParms (or /DP) is absent or fully direct —
// null, a dict, or an array of dicts/nulls. tspdf_stream_decode looks the
// parameters up per filter but does NOT resolve indirect references: a
// "/DecodeParms 6 0 R" (or a ref inside the array form) silently yields no
// parameters, so a predictor or LZW /EarlyChange 0 would be skipped and the
// "decoded" bytes would be wrong. The armor-strip path bakes those bytes
// into the output as plain Flate, so it must refuse any parms it cannot see.
static bool decode_parms_fully_direct(TspdfObj *dict) {
    TspdfObj *parms = tspdf_dict_get(dict, "DecodeParms");
    if (!parms) {
        parms = tspdf_dict_get(dict, "DP");
    }
    if (!parms || parms->type == TSPDF_OBJ_NULL || parms->type == TSPDF_OBJ_DICT) {
        return true;
    }
    if (parms->type == TSPDF_OBJ_ARRAY) {
        for (size_t i = 0; i < parms->array.count; i++) {
            TspdfObj *entry = &parms->array.items[i];
            if (entry->type != TSPDF_OBJ_DICT && entry->type != TSPDF_OBJ_NULL) {
                return false;
            }
        }
        return true;
    }
    return false;  // indirect reference or unexpected type
}

static uint16_t source_object_gen(TspdfReader *doc, uint32_t obj_num) {
    if (!doc || obj_num >= doc->xref.count) {
        return 0;
    }
    return doc->xref.entries[obj_num].gen;
}

static TspdfError get_stream_bytes(TspdfReader *doc, TspdfObj *obj,
                                   uint32_t src_obj_num, uint16_t src_gen,
                                   StreamBytes *out) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_STREAM || !out) {
        return TSPDF_ERR_PARSE;
    }

    out->data = NULL;
    out->len = 0;
    out->owned = NULL;

    if (obj->stream.self_contained && obj->stream.data != NULL) {
        out->data = obj->stream.data;
        out->len = obj->stream.len;
        return TSPDF_OK;
    }

    if (!doc->data || obj->stream.raw_offset > doc->data_len ||
        obj->stream.raw_len > doc->data_len - obj->stream.raw_offset) {
        return TSPDF_ERR_PARSE;
    }

    const uint8_t *raw = doc->data + obj->stream.raw_offset;
    size_t raw_len = obj->stream.raw_len;

    if (doc->crypt && src_obj_num > 0 && src_obj_num < doc->xref.count &&
        !stream_dict_type_is(obj, "XRef") && raw_len > 0 &&
        /* /EncryptMetadata false: the XMP metadata stream is stored in the
         * clear, so "decrypting" it would only garble it. */
        (doc->crypt->encrypt_metadata || !stream_dict_type_is(obj, "Metadata"))) {
        size_t decrypted_len = 0;
        uint8_t *decrypted = tspdf_crypt_decrypt_stream(doc->crypt, src_obj_num,
                                                        src_gen, raw, raw_len,
                                                        &decrypted_len);
        if (!decrypted) {
            return TSPDF_ERR_PARSE;
        }
        out->data = decrypted;
        out->len = decrypted_len;
        out->owned = decrypted;
        return TSPDF_OK;
    }

    out->data = raw;
    out->len = raw_len;
    return TSPDF_OK;
}

static bool is_serialized_page_object(TspdfReader *doc, uint32_t obj_num, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_DICT) {
        return false;
    }

    for (size_t i = 0; i < doc->pages.count; i++) {
        if (doc->pages.pages[i].obj_num == obj_num) {
            return true;
        }
    }

    TspdfObj *type_val = tspdf_dict_get(obj, "Type");
    return type_val && type_val->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)type_val->string.data, "Page") == 0;
}

// --- Helper: write stream with optional recompression ---
static TspdfError write_stream_obj_with_options(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                                TspdfReader *doc, const TspdfSaveOptions *opts,
                                                uint32_t src_obj_num, uint16_t src_gen) {
    TspdfObj *dict = obj->stream.dict;

    // Determine stream bytes source and length
    StreamBytes stream = {0};
    TspdfError err = get_stream_bytes(doc, obj, src_obj_num, src_gen, &stream);
    if (err != TSPDF_OK) {
        return err;
    }
    const uint8_t *stream_data = stream.data;
    size_t stream_len = stream.len;

    // Recompression. Every branch keeps whichever encoding is smaller, so a
    // stream never grows: an already-well-compressed stream stays verbatim.
    // The size-focused best-effort deflate level is used throughout —
    // `tspdf compress` is the only recompress_streams caller.
    uint8_t *recompressed = NULL;
    size_t recomp_len = 0;
    bool add_flate_filter = false;    // append /Filter /FlateDecode
    bool replace_filter = false;      // drop original /Filter + /DecodeParms too
    if (opts->recompress_streams && !stream_dict_type_is(obj, "XRef")) {
        TspdfObj *filter = tspdf_dict_get(dict, "Filter");
        bool is_image = stream_is_image(dict);
        if (filter && filter->type == TSPDF_OBJ_NAME &&
            strcmp((char *)filter->string.data, "FlateDecode") == 0) {
            // Flate stream (predictor params, if any, are preserved since
            // the re-encoded bytes are the same predicted bytes): inflate +
            // re-deflate and keep the smaller encoding. This recovers
            // streams stored at a low compression level (or as raw stored
            // blocks) regardless of their size.
            size_t dec_len = 0;
            uint8_t *decompressed = deflate_decompress(stream_data, stream_len, &dec_len);
            if (decompressed) {
                recompressed = is_image
                    ? deflate_compress(decompressed, dec_len, &recomp_len)
                    : deflate_compress_best(decompressed, dec_len, &recomp_len);
                free(decompressed);
                if (recompressed && recomp_len < stream_len) {
                    stream_data = recompressed;
                    stream_len = recomp_len;
                } else {
                    free(recompressed);
                    recompressed = NULL;
                }
            }
        } else if (!filter && stream_len > 0 &&
                   !tspdf_dict_get(dict, "FFilter") &&
                   !tspdf_dict_get(dict, "DecodeParms") &&
                   !tspdf_dict_get(dict, "DP") &&
                   !is_xmp_metadata(obj)) {
            // Stream stored with no filter at all: deflate it and add the
            // /Filter entry if that shrinks it. XMP metadata streams stay
            // uncompressed (PDF/A-style scanning), as does anything with
            // external-file or decode-parameter entries.
            recompressed = deflate_compress_best(stream_data, stream_len, &recomp_len);
            if (recompressed && recomp_len < stream_len) {
                stream_data = recompressed;
                stream_len = recomp_len;
                add_flate_filter = true;
            } else {
                free(recompressed);
                recompressed = NULL;
            }
        } else if (filter_chain_is_strippable(filter) &&
                   !tspdf_dict_get(dict, "FFilter") &&
                   decode_parms_fully_direct(dict)) {
            // ASCII/RunLength/LZW armor (possibly stacked on Flate): decode
            // the chain fully and re-encode as plain Flate when that is
            // smaller, replacing the whole /Filter (+/DecodeParms) — hex
            // doubles the bytes, ASCII85 adds 25%. Lossy/unknown filters
            // never reach here (see filter_chain_is_strippable), a failed
            // decode keeps the stream verbatim, and an indirect /DecodeParms
            // (which the decoder cannot resolve) refuses the strip entirely.
            uint8_t *decoded = NULL;
            size_t dec_len = 0;
            if (tspdf_stream_decode(dict, stream_data, stream_len,
                                    &decoded, &dec_len) == TSPDF_OK) {
                recompressed = is_image
                    ? deflate_compress(decoded, dec_len, &recomp_len)
                    : deflate_compress_best(decoded, dec_len, &recomp_len);
                free(decoded);
                if (recompressed && recomp_len < stream_len) {
                    stream_data = recompressed;
                    stream_len = recomp_len;
                    add_flate_filter = true;
                    replace_filter = true;
                } else {
                    free(recompressed);
                    recompressed = NULL;
                }
            }
        }
    }

    // Write dict with /Length overridden
    bool wrote_length = false;
    tspdf_buffer_append_str(buf, "<< ");
    for (size_t i = 0; i < dict->dict.count; i++) {
        const char *key = dict->dict.entries[i].key;
        if (replace_filter && (strcmp(key, "Filter") == 0 ||
                               strcmp(key, "DecodeParms") == 0 ||
                               strcmp(key, "DP") == 0)) {
            // The stream bytes were re-encoded as plain Flate; the original
            // filter chain (and its parameters) no longer applies.
            continue;
        }
        if (strcmp(key, "Length") == 0) {
            if (wrote_length) {
                continue;
            }
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            tspdf_buffer_printf(buf, "%zu", stream_len);
            wrote_length = true;
        } else {
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            write_obj(buf, dict->dict.entries[i].value, map, doc);
        }
        tspdf_buffer_append_byte(buf, ' ');
    }
    if (!wrote_length) {
        tspdf_buffer_printf(buf, "/Length %zu ", stream_len);
    }
    if (add_flate_filter) {
        tspdf_buffer_append_str(buf, "/Filter /FlateDecode ");
    }
    tspdf_buffer_append_str(buf, ">>");

    tspdf_buffer_append_str(buf, "\nstream\n");
    tspdf_buffer_append(buf, stream_data, stream_len);
    tspdf_buffer_append_str(buf, "\nendstream");

    free(recompressed);
    free(stream.owned);
    return buf->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

// --- Helper: check if object is XMP metadata ---
static bool is_xmp_metadata(TspdfObj *obj) {
    if (!obj) return false;
    TspdfObj *dict = NULL;
    if (obj->type == TSPDF_OBJ_STREAM) {
        dict = obj->stream.dict;
    } else if (obj->type == TSPDF_OBJ_DICT) {
        dict = obj;
    }
    if (!dict || dict->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *type_val = tspdf_dict_get(dict, "Type");
    return (type_val && type_val->type == TSPDF_OBJ_NAME &&
            strcmp((const char *)type_val->string.data, "Metadata") == 0);
}

// --- Object stream (ObjStm) packing ---
//
// Eligible (non-stream) objects are packed into /Type /ObjStm streams so
// thousands of small "N 0 obj … endobj" wrappers (plus their 20-byte xref
// entries) collapse into one Flate-compressed stream. Packing runs when
// recompress_streams asks for minimal output, and on any save whose INPUT
// used object streams (preserve-style; classic files are never force-packed).
// Everything this serializer writes has generation 0, the encryption
// dictionary is never packed (spec rule), and the xref stream itself is
// written separately.
//
// Encrypted saves pack too: `crypt` non-NULL encrypts each flushed ObjStm as
// one unit with the container's own object key. Member object bodies are
// serialized WITHOUT per-string encryption (ISO 32000 §7.5.7: strings in an
// object stream are protected by the stream's encryption, never individually).

enum { TSPDF_OBJSTM_CAPACITY = 128 };  // objects per ObjStm

typedef struct {
    TspdfBuffer *out;          // main output buffer
    size_t *offsets;           // top-level offsets, indexed by new object number
    uint32_t *objstm_of;       // ObjStm object number holding object i (0 = top-level)
    uint32_t *objstm_idx;      // index of object i within its ObjStm
    uint32_t next_objstm_num;  // object number the next flushed ObjStm gets
    uint32_t count;            // objects in the current (unflushed) ObjStm
    TspdfBuffer header;        // "objnum offset" pairs
    TspdfBuffer body;          // concatenated object bodies
    TspdfCrypt *crypt;         // non-NULL: encrypt flushed streams whole
    bool enabled;
    bool best;                 // best-effort deflate (size-focused saves)
} ObjStmWriter;

static bool objstm_writer_init(ObjStmWriter *w, TspdfBuffer *out, size_t *offsets,
                               uint32_t *objstm_of, uint32_t *objstm_idx,
                               uint32_t first_objstm_num, bool enabled,
                               TspdfCrypt *crypt, bool best) {
    memset(w, 0, sizeof(*w));
    w->out = out;
    w->offsets = offsets;
    w->objstm_of = objstm_of;
    w->objstm_idx = objstm_idx;
    w->next_objstm_num = first_objstm_num;
    w->enabled = enabled;
    w->crypt = crypt;
    w->best = best;
    if (enabled) {
        w->header = tspdf_buffer_create(1024);
        w->body = tspdf_buffer_create(16384);
        if (w->header.error || w->body.error) return false;
    }
    return true;
}

static void objstm_writer_destroy(ObjStmWriter *w) {
    if (w->enabled) {
        tspdf_buffer_destroy(&w->header);
        tspdf_buffer_destroy(&w->body);
    }
}

// Write the accumulated ObjStm as a top-level stream object.
static TspdfError objstm_writer_flush(ObjStmWriter *w) {
    if (!w->enabled || w->count == 0) return TSPDF_OK;
    if (w->header.error || w->body.error) return TSPDF_ERR_ALLOC;

    size_t first = w->header.len;
    size_t raw_len = first + w->body.len;
    uint8_t *raw = (uint8_t *)malloc(raw_len > 0 ? raw_len : 1);
    if (!raw) return TSPDF_ERR_ALLOC;
    memcpy(raw, w->header.data, w->header.len);
    memcpy(raw + first, w->body.data, w->body.len);

    size_t comp_len = 0;
    uint8_t *comp = w->best ? deflate_compress_best(raw, raw_len, &comp_len)
                            : deflate_compress(raw, raw_len, &comp_len);

    // Pick the smaller encoding, then (for encrypted saves) encrypt the whole
    // chosen payload with this container's own object key — member strings
    // were serialized in the clear on purpose (see the block comment above).
    const uint8_t *payload = raw;
    size_t payload_len = raw_len;
    bool flate = false;
    if (comp && comp_len < raw_len) {
        payload = comp;
        payload_len = comp_len;
        flate = true;
    }

    uint32_t num = w->next_objstm_num++;
    uint8_t *enc = NULL;
    if (w->crypt) {
        size_t enc_len = 0;
        enc = tspdf_crypt_encrypt_stream(w->crypt, num, 0, payload, payload_len, &enc_len);
        if (!enc) {
            free(comp);
            free(raw);
            return TSPDF_ERR_ALLOC;
        }
        payload = enc;
        payload_len = enc_len;
    }

    w->offsets[num] = w->out->len;
    tspdf_buffer_printf(w->out,
        "%u 0 obj\n<< /Type /ObjStm /N %u /First %zu /Length %zu%s >>\nstream\n",
        num, w->count, first, payload_len, flate ? " /Filter /FlateDecode" : "");
    tspdf_buffer_append(w->out, payload, payload_len);
    tspdf_buffer_append_str(w->out, "\nendstream\nendobj\n");
    free(enc);
    free(comp);
    free(raw);

    w->count = 0;
    tspdf_buffer_reset(&w->header);
    tspdf_buffer_reset(&w->body);
    return w->out->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

// Emit one non-stream object body: into the current ObjStm when packing is
// enabled, otherwise as a plain top-level "N 0 obj … endobj".
static TspdfError emit_nonstream_object(ObjStmWriter *w, uint32_t new_num,
                                        const TspdfBuffer *obj_body) {
    if (obj_body->error) return TSPDF_ERR_ALLOC;

    if (!w->enabled) {
        w->offsets[new_num] = w->out->len;
        tspdf_buffer_printf(w->out, "%u 0 obj\n", new_num);
        tspdf_buffer_append(w->out, obj_body->data, obj_body->len);
        tspdf_buffer_append_str(w->out, "\nendobj\n");
        return w->out->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
    }

    tspdf_buffer_printf(&w->header, "%u %zu ", new_num, w->body.len);
    tspdf_buffer_append(&w->body, obj_body->data, obj_body->len);
    tspdf_buffer_append_byte(&w->body, '\n');  // whitespace between objects
    if (w->header.error || w->body.error) return TSPDF_ERR_ALLOC;
    w->objstm_of[new_num] = w->next_objstm_num;
    w->objstm_idx[new_num] = w->count;
    w->count++;
    if (w->count >= TSPDF_OBJSTM_CAPACITY) return objstm_writer_flush(w);
    return TSPDF_OK;
}

// --- Helper: write xref stream ---

// Minimal number of big-endian bytes needed to store v.
static int xref_field_width(uint64_t v) {
    int w = 1;
    while (v > 0xFF) {
        v >>= 8;
        w++;
    }
    return w;
}

// objstm_of/objstm_idx (may be NULL): objstm_of[i] != 0 means object i lives
// in object stream objstm_of[i] at index objstm_idx[i] (type-2 entry).
// encrypt_obj_num != 0 adds /Encrypt and /ID trailer keys for encrypted
// saves; the xref stream itself is never encrypted (ISO 32000 §7.5.8.2 —
// readers must parse it before any key is available), so writing it here
// with plain deflate is correct even when the rest of the file is encrypted.
static TspdfError write_xref_stream(TspdfBuffer *buf, size_t *offsets, uint32_t total_objects,
                                      uint32_t root_ref, uint32_t info_obj_num,
                                      bool write_info, const TspdfSaveOptions *opts,
                                      const uint32_t *objstm_of, const uint32_t *objstm_idx,
                                      uint32_t encrypt_obj_num,
                                      const uint8_t *file_id, size_t file_id_len) {
    // The xref stream is written as a new object (total_objects + 1)
    uint32_t xref_obj_num = total_objects + 1;
    size_t xref_stream_offset = buf->len;
    uint32_t size_val = xref_obj_num + 1;  // /Size is highest object number + 1

    // Right-size /W instead of a fixed [1 8 2] (11 bytes/entry). Field 1 is
    // the entry type (1 byte). Field 2 holds byte offsets (type 1) or
    // object-stream numbers (type 2); the xref stream's own offset is the
    // largest offset since it is written last. Field 3 holds the index
    // within an object stream for type-2 entries; free entries write 0 there
    // (as qpdf does — a spec-literal 65535 generation would force a 2-byte
    // field 3 on every file for no reader benefit).
    uint64_t max_f2 = xref_stream_offset;
    uint64_t max_f3 = 0;
    for (uint32_t i = 1; i <= total_objects; i++) {
        if (objstm_of && objstm_of[i] != 0) {
            if (objstm_of[i] > max_f2) max_f2 = objstm_of[i];
            if (objstm_idx[i] > max_f3) max_f3 = objstm_idx[i];
        }
    }
    const int w2 = xref_field_width(max_f2);
    const int w3 = xref_field_width(max_f3);
    const size_t entry_size = (size_t)(1 + w2 + w3);

    size_t entry_count = (size_t)size_val;  // 0..xref_obj_num
    if (entry_count > SIZE_MAX / entry_size) {
        return TSPDF_ERR_UNSUPPORTED;
    }
    size_t raw_len = entry_count * entry_size;
    uint8_t *raw = (uint8_t *)calloc(raw_len, 1);
    if (!raw) return TSPDF_ERR_ALLOC;

    // Entry 0 stays all-zero: type 0 (free), next free 0.
    for (uint32_t i = 1; i <= xref_obj_num; i++) {
        uint8_t *e = raw + (size_t)i * entry_size;
        uint64_t f2, f3;

        if (i == xref_obj_num) {
            e[0] = 1;  // the xref stream itself
            f2 = (uint64_t)xref_stream_offset;
            f3 = 0;
        } else if (objstm_of && objstm_of[i] != 0) {
            e[0] = 2;  // compressed in an object stream
            f2 = objstm_of[i];
            f3 = objstm_idx[i];
        } else if (offsets[i] != 0) {
            e[0] = 1;  // in-use, top-level
            f2 = (uint64_t)offsets[i];
            f3 = 0;
        } else {
            continue;  // free entry: all zero
        }

        for (int b = 0; b < w2; b++) {
            e[1 + b] = (uint8_t)(f2 >> (8 * (w2 - 1 - b)));
        }
        for (int b = 0; b < w3; b++) {
            e[1 + w2 + b] = (uint8_t)(f3 >> (8 * (w3 - 1 - b)));
        }
    }

    // Compress
    size_t comp_len = 0;
    uint8_t *compressed = deflate_compress(raw, raw_len, &comp_len);
    free(raw);
    if (!compressed) return TSPDF_ERR_ALLOC;

    // Write xref stream object
    tspdf_buffer_printf(buf, "%u 0 obj\n<< /Type /XRef /Size %u /W [ 1 %d %d ] /Root %u 0 R",
                  xref_obj_num, size_val, w2, w3, root_ref);
    if (write_info && (!opts || !opts->strip_metadata)) {
        tspdf_buffer_printf(buf, " /Info %u 0 R", info_obj_num);
    }
    if (encrypt_obj_num != 0) {
        tspdf_buffer_printf(buf, " /Encrypt %u 0 R /ID [ ", encrypt_obj_num);
        write_hex_string(buf, file_id, file_id_len);
        tspdf_buffer_append_byte(buf, ' ');
        write_hex_string(buf, file_id, file_id_len);
        tspdf_buffer_append_str(buf, " ]");
    }
    tspdf_buffer_printf(buf, " /Length %zu /Filter /FlateDecode >>\nstream\n", comp_len);
    tspdf_buffer_append(buf, compressed, comp_len);
    tspdf_buffer_append_str(buf, "\nendstream\nendobj\n");
    free(compressed);

    tspdf_buffer_printf(buf, "startxref\n%zu\n%%%%EOF\n", xref_stream_offset);
    return buf->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

TspdfError tspdf_serialize_with_options(TspdfReader *doc, uint8_t **out_buf, size_t *out_len,
                                      const TspdfSaveOptions *opts) {
    if (!doc || !out_buf || !out_len || !opts) return TSPDF_ERR_PARSE;

    // A document opened with a password stays encrypted on save: silently
    // writing it decrypted would strip its protection behind the caller's
    // back. The encrypted writer reuses the recovered file key and copies the
    // source /Encrypt dict and /ID verbatim, so both original passwords keep
    // working. opts->decrypt is the explicit opt-out (`tspdf decrypt`). This
    // path ignores recompress options; the encrypted writer re-packs object
    // streams on its own when the input used them (see tspdf_serialize_encrypted).
    if (doc->crypt && !opts->decrypt) {
        return tspdf_serialize_encrypted(doc, doc->crypt, out_buf, out_len);
    }

    // Inputs that used object streams are re-packed on save (preserve-style);
    // that requires type-2 entries and therefore an xref stream.
    bool repack_objstm = input_uses_objstm(doc);

    // recompress_streams targets minimal output size, so it also implies the
    // compact compressed xref stream over the classic table (whose 20 bytes
    // per object dominate the achievable savings on object-heavy files).
    bool use_xref_stream = opts->use_xref_stream || opts->recompress_streams ||
                           repack_objstm;

    // --- Preserve object IDs fast path ---
    // When preserve_object_ids is set and document is unmodified, copy raw
    // bytes. Not possible for object-stream inputs: type-2 entries have no
    // byte offset to copy from, so those go through the standard path below.
    bool use_preserve = opts->preserve_object_ids && !opts->strip_metadata &&
                        !doc->modified && doc->data != NULL
                        && doc->new_objs.count == 0 && !repack_objstm;

    // Set up a parser for resolving objects
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *source_catalog = catalog_for_serialize(doc, &parser);

    // 1. Collect all reachable objects
    size_t total_objs = doc->xref.count + doc->new_objs.count;
    bool *visited = (bool *)calloc(total_objs, sizeof(bool));
    if (!visited) return TSPDF_ERR_ALLOC;

    uint32_t source_root_num = 0;
    uint32_t source_info_num = 0;
    if (doc->xref.trailer) {
        TspdfObj *root_ref = tspdf_dict_get(doc->xref.trailer, "Root");
        if (root_ref && root_ref->type == TSPDF_OBJ_REF &&
            root_ref->ref.num < total_objs) {
            source_root_num = root_ref->ref.num;
        }
        TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
        if (info_ref && info_ref->type == TSPDF_OBJ_REF &&
            info_ref->ref.num < total_objs) {
            source_info_num = info_ref->ref.num;
        }
    }

    // Decide collection strategy
    bool use_fast_collect = !doc->modified && doc->new_objs.count == 0
                            && !opts->strip_unused_objects;

    if (use_fast_collect) {
        for (size_t i = 1; i < doc->xref.count; i++) {
            if (doc->xref.entries[i].in_use) {
                visited[i] = true;
            }
        }
    } else {
        // Slow path: recursive walk from pages
        for (size_t i = 0; i < doc->pages.count; i++) {
            TspdfReaderPage *page = &doc->pages.pages[i];
            if (page->obj_num < total_objs) {
                visited[page->obj_num] = true;
            }
            collect_from_page_obj(doc, page->page_dict, visited, &parser);
        }
        collect_from_catalog_entries(doc, source_catalog, visited, &parser,
                                     opts->strip_metadata);
    }

    if (use_preserve) {
        // ========== PRESERVE OBJECT IDS PATH ==========
        // Copy raw bytes from source, preserving original object numbers.

        // Find the original /Root and /Info refs from trailer
        uint32_t orig_root_num = 0;
        uint32_t orig_info_num = 0;
        if (doc->xref.trailer) {
            TspdfObj *root_ref = tspdf_dict_get(doc->xref.trailer, "Root");
            if (root_ref && root_ref->type == TSPDF_OBJ_REF)
                orig_root_num = root_ref->ref.num;
            TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
            if (info_ref && info_ref->type == TSPDF_OBJ_REF)
                orig_info_num = info_ref->ref.num;
        }

        if (orig_root_num > 0 && orig_root_num < total_objs) {
            visited[orig_root_num] = true;
            collect_from_obj(doc, source_catalog, visited, &parser);
        }

        // Find highest object number
        uint32_t max_obj = 0;
        for (size_t i = 1; i < total_objs; i++) {
            if (visited[i] && (uint32_t)i > max_obj) max_obj = (uint32_t)i;
        }

        // Handle update_producer: may need to create/update Info dict
        bool need_producer_info = opts->update_producer && !opts->strip_metadata;
        uint32_t producer_info_num = 0;
        if (need_producer_info) {
            if (orig_info_num > 0) {
                producer_info_num = orig_info_num;
                // We'll re-serialize this one object (not raw copy)
            } else {
                // Create a new Info object
                producer_info_num = max_obj + 1;
                max_obj = producer_info_num;
            }
        }

        // Allocate offset tracking (indexed by original object number)
        size_t *offsets = (size_t *)calloc(max_obj + 1, sizeof(size_t));
        if (!offsets) { free(visited); return TSPDF_ERR_ALLOC; }

        // Determine PDF version
        char version[8];
        memcpy(version, doc->pdf_version, sizeof(version));
        if (use_xref_stream) {
            // Ensure at least 1.5
            if (strcmp(version, "1.0") == 0 || strcmp(version, "1.1") == 0 ||
                strcmp(version, "1.2") == 0 || strcmp(version, "1.3") == 0 ||
                strcmp(version, "1.4") == 0) {
                memcpy(version, "1.5", 4);
            }
        }

        TspdfBuffer buf = tspdf_buffer_create(doc->data_len);
        if (buf.error) { free(offsets); free(visited); return TSPDF_ERR_ALLOC; }

        // Header
        tspdf_buffer_printf(&buf, "%%PDF-%s\n", version);
        tspdf_buffer_append_byte(&buf, '%');
        tspdf_buffer_append_byte(&buf, 0xE2);
        tspdf_buffer_append_byte(&buf, 0xE3);
        tspdf_buffer_append_byte(&buf, 0xCF);
        tspdf_buffer_append_byte(&buf, 0xD3);
        tspdf_buffer_append_byte(&buf, '\n');

        // Write objects: raw byte copy from source
        for (size_t i = 1; i < doc->xref.count; i++) {
            if (!visited[i]) continue;

            // Skip metadata objects if strip_metadata
            if (opts->strip_metadata) {
                if (orig_info_num > 0 && (uint32_t)i == orig_info_num) continue;
                TspdfObj *obj = tspdf_xref_resolve(&doc->xref, &parser, (uint32_t)i,
                                                  doc->obj_cache, NULL);
                if (is_xmp_metadata(obj)) continue;
            }

            // Skip Info object if we're rewriting it for update_producer
            if (need_producer_info && (uint32_t)i == orig_info_num) {
                // Will be written separately below
                continue;
            }

            size_t obj_start = doc->xref.entries[i].offset;
            size_t obj_end = find_endobj(doc->data, doc->data_len, obj_start);

            offsets[i] = buf.len;
            tspdf_buffer_append(&buf, doc->data + obj_start, obj_end - obj_start);
            tspdf_buffer_append_byte(&buf, '\n');
        }

        // Write producer Info dict if needed
        if (need_producer_info) {
            offsets[producer_info_num] = buf.len;
            // Build a minimal or updated Info dict with /Producer
            tspdf_buffer_printf(&buf, "%u 0 obj\n<< ", producer_info_num);

            // If there was an original Info dict, copy its fields (except Producer)
            if (orig_info_num > 0) {
                TspdfObj *orig_info = tspdf_xref_resolve(&doc->xref, &parser, orig_info_num,
                                                        doc->obj_cache, NULL);
                if (orig_info && orig_info->type == TSPDF_OBJ_DICT) {
                    // We need an identity map for write_obj since we preserve IDs
                    RenumberMap identity_map;
                    identity_map.map_size = total_objs;
                    identity_map.old_to_new = (uint32_t *)calloc(total_objs, sizeof(uint32_t));
                    if (identity_map.old_to_new) {
                        for (size_t k = 0; k < total_objs; k++)
                            identity_map.old_to_new[k] = (uint32_t)k;
                        for (size_t j = 0; j < orig_info->dict.count; j++) {
                            const char *key = orig_info->dict.entries[j].key;
                            if (strcmp(key, "Producer") == 0) continue;
                            write_name(&buf, (const uint8_t *)key, strlen(key));
                            tspdf_buffer_append_byte(&buf, ' ');
                            write_obj(&buf, orig_info->dict.entries[j].value, &identity_map, doc);
                            tspdf_buffer_append_byte(&buf, ' ');
                        }
                        free(identity_map.old_to_new);
                    }
                }
            }
            tspdf_buffer_append_str(&buf, "/Producer (tspdf " TSPDF_VERSION_STRING ") >>\nendobj\n");
        }

        // Write xref / trailer
        if (use_xref_stream) {
            uint32_t root_for_trailer = orig_root_num;
            uint32_t info_for_trailer = (!opts->strip_metadata && need_producer_info) ? producer_info_num : 0;
            TspdfError xref_err = write_xref_stream(&buf, offsets, max_obj, root_for_trailer,
                                                     info_for_trailer, info_for_trailer > 0, opts,
                                                     NULL, NULL, 0, NULL, 0);
            if (xref_err != TSPDF_OK) {
                tspdf_buffer_destroy(&buf);
                free(offsets);
                free(visited);
                return xref_err;
            }
        } else {
            size_t xref_offset = buf.len;
            tspdf_buffer_printf(&buf, "xref\n0 %u\n", max_obj + 1);
            tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
            for (uint32_t i = 1; i <= max_obj; i++) {
                if (offsets[i] > 0) {
                    tspdf_buffer_printf(&buf, "%010zu 00000 n \n", offsets[i]);
                } else {
                    tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
                }
            }

            tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root %u 0 R",
                          max_obj + 1, orig_root_num);
            if (!opts->strip_metadata && need_producer_info) {
                tspdf_buffer_printf(&buf, " /Info %u 0 R", producer_info_num);
            } else if (!opts->strip_metadata && orig_info_num > 0) {
                tspdf_buffer_printf(&buf, " /Info %u 0 R", orig_info_num);
            }
            tspdf_buffer_append_str(&buf, " >>\n");
            tspdf_buffer_printf(&buf, "startxref\n%zu\n%%%%EOF\n", xref_offset);
        }

        if (buf.error) {
            tspdf_buffer_destroy(&buf);
            free(offsets);
            free(visited);
            return TSPDF_ERR_ALLOC;
        }

        *out_buf = buf.data;
        *out_len = buf.len;
        free(offsets);
        free(visited);
        return TSPDF_OK;
    }

    // ========== STANDARD RE-SERIALIZATION PATH ==========

    // 2. Build renumbering map
    RenumberMap map;
    map.map_size = total_objs;
    map.old_to_new = (uint32_t *)calloc(map.map_size, sizeof(uint32_t));
    if (!map.old_to_new) {
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    uint32_t *collected = NULL;
    size_t collected_count = 0;
    {
        for (size_t i = 1; i < total_objs; i++) {
            if (!visited[i]) continue;
            if (!use_preserve && source_root_num > 0 &&
                (uint32_t)i == source_root_num) {
                continue;
            }
            // Source ObjStm/XRef containers are dropped: the fast-collect
            // path marks every in-use entry, which includes them, and copying
            // them would duplicate every object they hold (the members are
            // written individually below).
            if (is_xref_machinery(resolve_for_collect(doc, (uint32_t)i, &parser))) {
                continue;
            }
            if (opts->strip_metadata) {
                if (source_info_num > 0 && (uint32_t)i == source_info_num) {
                    continue;
                }
                TspdfObj *obj = resolve_for_collect(doc, (uint32_t)i, &parser);
                if (is_xmp_metadata(obj)) {
                    continue;
                }
            }
            collected_count++;
        }
        collected = (uint32_t *)malloc(sizeof(uint32_t) * (collected_count > 0 ? collected_count : 1));
        if (!collected) {
            free(map.old_to_new);
            free(visited);
            return TSPDF_ERR_ALLOC;
        }
        size_t ci = 0;
        for (size_t i = 1; i < total_objs; i++) {
            if (!visited[i]) continue;
            if (!use_preserve && source_root_num > 0 &&
                (uint32_t)i == source_root_num) {
                continue;
            }
            if (is_xref_machinery(resolve_for_collect(doc, (uint32_t)i, &parser))) {
                continue;
            }
            if (opts->strip_metadata) {
                if (source_info_num > 0 && (uint32_t)i == source_info_num) {
                    continue;
                }
                TspdfObj *obj = resolve_for_collect(doc, (uint32_t)i, &parser);
                if (is_xmp_metadata(obj)) {
                    continue;
                }
            }
            collected[ci++] = (uint32_t)i;
        }
    }

    // New obj 1 = Catalog (synthetic), new obj 2 = Pages (synthetic)
    uint32_t next_num = 3;
    for (size_t i = 0; i < collected_count; i++) {
        map.old_to_new[collected[i]] = next_num++;
    }

    // Determine if we need an Info object
    bool write_info = false;
    if (!opts->strip_metadata) {
        write_info = metadata_has_changes(doc->metadata) || opts->update_producer;
    }
    uint32_t info_obj_num = 0;
    if (write_info) {
        info_obj_num = next_num++;
    }

    uint32_t total_objects = next_num - 1;

    // Determine PDF version
    char version[8];
    memcpy(version, doc->pdf_version, sizeof(version));
    if (use_xref_stream) {
        if (strcmp(version, "1.0") == 0 || strcmp(version, "1.1") == 0 ||
            strcmp(version, "1.2") == 0 || strcmp(version, "1.3") == 0 ||
            strcmp(version, "1.4") == 0) {
            memcpy(version, "1.5", 4);
        }
    }

    // Object-stream packing: when recompression asks for minimal output, or
    // when the input itself used object streams (unpacking those into classic
    // objects roughly doubles such files). Both cases imply use_xref_stream,
    // which type-2 entries require. This path always writes unencrypted
    // output (encrypted saves go through tspdf_serialize_encrypted, which has
    // its own packing writer).
    bool use_objstm = opts->recompress_streams || repack_objstm;
    uint32_t max_objstms = use_objstm
        ? total_objects / TSPDF_OBJSTM_CAPACITY + 2 : 0;
    size_t table_size = (size_t)total_objects + max_objstms + 2;

    // 3. Write PDF
    TspdfBuffer buf = tspdf_buffer_create(doc->data_len > 0 ? doc->data_len : 65536);
    if (buf.error) {
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    // Header
    tspdf_buffer_printf(&buf, "%%PDF-%s\n", version);
    tspdf_buffer_append_byte(&buf, '%');
    tspdf_buffer_append_byte(&buf, 0xE2);
    tspdf_buffer_append_byte(&buf, 0xE3);
    tspdf_buffer_append_byte(&buf, 0xCF);
    tspdf_buffer_append_byte(&buf, 0xD3);
    tspdf_buffer_append_byte(&buf, '\n');

    // Track offsets for xref, plus type-2 locations when packing
    size_t *offsets = (size_t *)calloc(table_size, sizeof(size_t));
    uint32_t *objstm_of = use_objstm ? (uint32_t *)calloc(table_size, sizeof(uint32_t)) : NULL;
    uint32_t *objstm_idx = use_objstm ? (uint32_t *)calloc(table_size, sizeof(uint32_t)) : NULL;
    // Scratch buffer for one non-stream object body at a time
    TspdfBuffer scratch = tspdf_buffer_create(4096);
    ObjStmWriter stm_writer;
    bool stm_writer_ok = objstm_writer_init(&stm_writer, &buf, offsets, objstm_of,
                                            objstm_idx, total_objects + 1, use_objstm,
                                            NULL, opts->recompress_streams);
    if (!offsets || scratch.error || !stm_writer_ok ||
        (use_objstm && (!objstm_of || !objstm_idx))) {
        objstm_writer_destroy(&stm_writer);
        tspdf_buffer_destroy(&scratch);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    TspdfError emit_err = TSPDF_OK;

    // Object 1: Catalog
    tspdf_buffer_reset(&scratch);
    write_catalog_body(&scratch, source_catalog, &map, doc, opts->strip_metadata);
    emit_err = emit_nonstream_object(&stm_writer, 1, &scratch);

    // Object 2: Pages
    if (emit_err == TSPDF_OK) {
        tspdf_buffer_reset(&scratch);
        tspdf_buffer_append_str(&scratch, "<< /Type /Pages /Kids [ ");
        for (size_t i = 0; i < doc->pages.count; i++) {
            uint32_t old_num = doc->pages.pages[i].obj_num;
            uint32_t new_num = map.old_to_new[old_num];
            if (i > 0) tspdf_buffer_append_byte(&scratch, ' ');
            tspdf_buffer_printf(&scratch, "%u 0 R", new_num);
        }
        tspdf_buffer_printf(&scratch, " ] /Count %zu >>", doc->pages.count);
        emit_err = emit_nonstream_object(&stm_writer, 2, &scratch);
    }

    // Objects 3..total_objects: the collected objects
    for (size_t i = 0; emit_err == TSPDF_OK && i < collected_count; i++) {
        uint32_t old_num = collected[i];
        uint32_t new_num = map.old_to_new[old_num];
        TspdfObj *obj = resolve_for_collect(doc, old_num, &parser);
        if (!obj) continue;

        // Skip metadata objects if strip_metadata
        if (opts->strip_metadata && is_xmp_metadata(obj)) continue;

        if (obj->type == TSPDF_OBJ_STREAM) {
            // Streams are never packed into object streams (spec rule);
            // always written top-level.
            offsets[new_num] = buf.len;
            tspdf_buffer_printf(&buf, "%u 0 obj\n", new_num);
            emit_err = write_stream_obj_with_options(
                &buf, obj, &map, doc, opts, old_num, source_object_gen(doc, old_num));
            tspdf_buffer_append_str(&buf, "\nendobj\n");
            continue;
        }

        tspdf_buffer_reset(&scratch);
        write_nonstream_body_plain(&scratch, doc, obj, old_num, &map);
        emit_err = emit_nonstream_object(&stm_writer, new_num, &scratch);
    }

    // Info dict object. Kept top-level even when packing: write_info_dict_obj
    // emits a complete indirect object, and one small dict does not change
    // the size math.
    if (emit_err == TSPDF_OK && write_info) {
        offsets[info_obj_num] = buf.len;
        if (metadata_has_changes(doc->metadata)) {
            write_info_dict_obj(&buf, doc, &parser, info_obj_num, NULL);
        } else if (opts->update_producer) {
            // Just write a simple Info dict with /Producer
            tspdf_buffer_printf(&buf, "%u 0 obj\n<< /Producer (tspdf " TSPDF_VERSION_STRING ") >>\nendobj\n", info_obj_num);
        }
    }

    // Flush the last (partial) object stream
    if (emit_err == TSPDF_OK) {
        emit_err = objstm_writer_flush(&stm_writer);
    }
    // Object streams get numbers above the regular objects
    uint32_t total_with_objstms = use_objstm ? stm_writer.next_objstm_num - 1
                                             : total_objects;
    objstm_writer_destroy(&stm_writer);
    tspdf_buffer_destroy(&scratch);

    if (emit_err != TSPDF_OK) {
        tspdf_buffer_destroy(&buf);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return emit_err;
    }

    // Xref table or xref stream
    if (use_xref_stream) {
        TspdfError xref_err = write_xref_stream(&buf, offsets, total_with_objstms, 1,
                                                 info_obj_num, write_info, opts,
                                                 objstm_of, objstm_idx, 0, NULL, 0);
        if (xref_err != TSPDF_OK) {
            tspdf_buffer_destroy(&buf);
            free(objstm_idx);
            free(objstm_of);
            free(offsets);
            free(collected);
            free(map.old_to_new);
            free(visited);
            return xref_err;
        }
    } else {
        size_t xref_offset = buf.len;
        tspdf_buffer_printf(&buf, "xref\n0 %u\n", total_objects + 1);
        tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
        for (uint32_t i = 1; i <= total_objects; i++) {
            if (offsets[i] > 0) {
                tspdf_buffer_printf(&buf, "%010zu 00000 n \n", offsets[i]);
            } else {
                // The object was collected but never written (e.g. a dangling
                // in-use source entry that failed to resolve). An in-use
                // (type 1) entry with offset 0 is structurally invalid; a
                // free entry lets refs to it resolve to null instead.
                tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
            }
        }

        if (write_info) {
            tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root 1 0 R /Info %u 0 R >>\n",
                          total_objects + 1, info_obj_num);
        } else {
            tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root 1 0 R >>\n", total_objects + 1);
        }
        tspdf_buffer_printf(&buf, "startxref\n%zu\n%%%%EOF\n", xref_offset);
    }

    if (buf.error) {
        tspdf_buffer_destroy(&buf);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    *out_buf = buf.data;
    *out_len = buf.len;

    free(objstm_idx);
    free(objstm_of);
    free(offsets);
    free(collected);
    free(map.old_to_new);
    free(visited);

    return TSPDF_OK;
}

/* --- Encrypted serialization helpers --- */

static void write_hex_string(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_buffer_append_byte(buf, '<');
    for (size_t i = 0; i < len; i++) {
        tspdf_buffer_printf(buf, "%02X", data[i]);
    }
    tspdf_buffer_append_byte(buf, '>');
}

static void write_obj_encrypted(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                TspdfReader *doc, TspdfCrypt *crypt,
                                uint32_t cur_obj_num, uint16_t cur_gen);

static void write_obj_encrypted(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                TspdfReader *doc, TspdfCrypt *crypt,
                                uint32_t cur_obj_num, uint16_t cur_gen) {
    if (!obj) {
        tspdf_buffer_append_str(buf, "null");
        return;
    }

    switch (obj->type) {
        case TSPDF_OBJ_NULL:
            tspdf_buffer_append_str(buf, "null");
            break;
        case TSPDF_OBJ_BOOL:
            tspdf_buffer_append_str(buf, obj->boolean ? "true" : "false");
            break;
        case TSPDF_OBJ_INT:
            tspdf_buffer_printf(buf, "%lld", (long long)obj->integer);
            break;
        case TSPDF_OBJ_REAL: {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.6f", obj->real);
            char *dot = strchr(tmp, '.');
            if (dot) {
                char *end = tmp + strlen(tmp) - 1;
                while (end > dot && *end == '0') end--;
                if (end == dot) end--;
                end[1] = '\0';
            }
            tspdf_buffer_append_str(buf, tmp);
            break;
        }
        case TSPDF_OBJ_STRING: {
            /* Encrypt the string value */
            size_t enc_len = 0;
            uint8_t *enc = tspdf_crypt_encrypt_string(crypt, cur_obj_num, cur_gen,
                                                      obj->string.data, obj->string.len, &enc_len);
            if (enc) {
                write_hex_string(buf, enc, enc_len);
                free(enc);
            } else {
                write_hex_string(buf, obj->string.data, obj->string.len);
            }
            break;
        }
        case TSPDF_OBJ_NAME:
            write_name(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            tspdf_buffer_append_str(buf, "[ ");
            for (size_t i = 0; i < obj->array.count; i++) {
                if (i > 0) tspdf_buffer_append_byte(buf, ' ');
                write_obj_encrypted(buf, &obj->array.items[i], map, doc, crypt, cur_obj_num, cur_gen);
            }
            tspdf_buffer_append_str(buf, " ]");
            break;
        case TSPDF_OBJ_DICT:
            tspdf_buffer_append_str(buf, "<< ");
            for (size_t i = 0; i < obj->dict.count; i++) {
                write_name(buf, (const uint8_t *)obj->dict.entries[i].key,
                           strlen(obj->dict.entries[i].key));
                tspdf_buffer_append_byte(buf, ' ');
                write_obj_encrypted(buf, obj->dict.entries[i].value, map, doc, crypt, cur_obj_num, cur_gen);
                tspdf_buffer_append_byte(buf, ' ');
            }
            tspdf_buffer_append_str(buf, ">>");
            break;
        case TSPDF_OBJ_STREAM:
            break;
        case TSPDF_OBJ_REF: {
            uint32_t new_num = 0;
            if (obj->ref.num < map->map_size) {
                new_num = map->old_to_new[obj->ref.num];
            }
            if (new_num == 0) {
                tspdf_buffer_append_str(buf, "null");
            } else {
                tspdf_buffer_printf(buf, "%u 0 R", new_num);
            }
            break;
        }
    }
}

// Write a preserved source /Encrypt dictionary verbatim: strings (O/U/OE/UE/
// Perms...) as raw hex, never object-key encrypted (the /Encrypt dict is the
// one place the spec exempts). References are resolved and inlined — the spec
// requires /Encrypt entries to be direct, but a lenient copy beats emitting a
// dangling reference into the renumbered file. `depth` guards hostile cycles.
static void write_encrypt_dict_plain(TspdfBuffer *buf, TspdfObj *obj,
                                     TspdfReader *doc, TspdfParser *parser,
                                     int depth) {
    if (!obj || depth > 32) {
        tspdf_buffer_append_str(buf, "null");
        return;
    }

    switch (obj->type) {
        case TSPDF_OBJ_NULL:
            tspdf_buffer_append_str(buf, "null");
            break;
        case TSPDF_OBJ_BOOL:
            tspdf_buffer_append_str(buf, obj->boolean ? "true" : "false");
            break;
        case TSPDF_OBJ_INT:
            tspdf_buffer_printf(buf, "%lld", (long long)obj->integer);
            break;
        case TSPDF_OBJ_REAL:
            tspdf_buffer_printf(buf, "%.6f", obj->real);
            break;
        case TSPDF_OBJ_STRING:
            write_hex_string(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_NAME:
            write_name(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            tspdf_buffer_append_str(buf, "[ ");
            for (size_t i = 0; i < obj->array.count; i++) {
                if (i > 0) tspdf_buffer_append_byte(buf, ' ');
                write_encrypt_dict_plain(buf, &obj->array.items[i], doc,
                                         parser, depth + 1);
            }
            tspdf_buffer_append_str(buf, " ]");
            break;
        case TSPDF_OBJ_DICT:
            tspdf_buffer_append_str(buf, "<< ");
            for (size_t i = 0; i < obj->dict.count; i++) {
                write_name(buf, (const uint8_t *)obj->dict.entries[i].key,
                           strlen(obj->dict.entries[i].key));
                tspdf_buffer_append_byte(buf, ' ');
                write_encrypt_dict_plain(buf, obj->dict.entries[i].value, doc,
                                         parser, depth + 1);
                tspdf_buffer_append_byte(buf, ' ');
            }
            tspdf_buffer_append_str(buf, ">>");
            break;
        case TSPDF_OBJ_REF:
            write_encrypt_dict_plain(buf,
                resolve_for_collect(doc, obj->ref.num, parser), doc, parser,
                depth + 1);
            break;
        case TSPDF_OBJ_STREAM:
            tspdf_buffer_append_str(buf, "null");
            break;
    }
}

static void write_catalog_obj_encrypted(TspdfBuffer *buf, TspdfObj *catalog,
                                        const RenumberMap *map, TspdfReader *doc,
                                        TspdfCrypt *crypt) {
    bool wrote_type = false;

    tspdf_buffer_append_str(buf, "1 0 obj\n<< ");
    if (catalog && catalog->type == TSPDF_OBJ_DICT) {
        for (size_t i = 0; i < catalog->dict.count; i++) {
            const char *key = catalog->dict.entries[i].key;
            if (!preserve_catalog_key(key, false)) {
                continue;
            }

            if (strcmp(key, "Type") == 0) {
                wrote_type = true;
            }
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            write_obj_encrypted(buf, catalog->dict.entries[i].value, map, doc,
                                crypt, 1, 0);
            tspdf_buffer_append_byte(buf, ' ');
        }
    }

    if (!wrote_type) {
        tspdf_buffer_append_str(buf, "/Type /Catalog ");
    }
    tspdf_buffer_append_str(buf, "/Pages 2 0 R >>\nendobj\n");
}

static TspdfError write_stream_obj_encrypted(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                             TspdfReader *doc, TspdfCrypt *crypt,
                                             uint32_t src_obj_num, uint16_t src_gen,
                                             uint32_t cur_obj_num, uint16_t cur_gen) {
    TspdfObj *dict = obj->stream.dict;

    /* Get stream raw data */
    StreamBytes stream = {0};
    TspdfError err = get_stream_bytes(doc, obj, src_obj_num, src_gen, &stream);
    if (err != TSPDF_OK) {
        return err;
    }

    /* Encrypt the stream data. Exception: when the (preserved) crypt says
     * /EncryptMetadata false, the XMP metadata stream is stored in the clear
     * (ISO 32000 §7.6.3.2); encrypting it anyway would garble it for readers
     * honoring the flag. */
    bool skip_encrypt =
        !crypt->encrypt_metadata && stream_dict_type_is(obj, "Metadata");
    size_t enc_len = 0;
    uint8_t *enc = skip_encrypt
        ? NULL
        : tspdf_crypt_encrypt_stream(crypt, cur_obj_num, cur_gen,
                                     stream.data, stream.len, &enc_len);
    const uint8_t *write_data = enc ? enc : stream.data;
    size_t write_len = enc ? enc_len : stream.len;

    /* Write dict with /Length overridden and strings encrypted */
    bool wrote_length = false;
    tspdf_buffer_append_str(buf, "<< ");
    for (size_t i = 0; i < dict->dict.count; i++) {
        const char *key = dict->dict.entries[i].key;
        if (strcmp(key, "Length") == 0) {
            if (wrote_length) {
                continue;
            }
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            tspdf_buffer_printf(buf, "%zu", write_len);
            wrote_length = true;
        } else {
            write_name(buf, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(buf, ' ');
            write_obj_encrypted(buf, dict->dict.entries[i].value, map, doc, crypt, cur_obj_num, cur_gen);
        }
        tspdf_buffer_append_byte(buf, ' ');
    }
    if (!wrote_length) {
        tspdf_buffer_printf(buf, "/Length %zu ", write_len);
    }
    tspdf_buffer_append_str(buf, ">>");

    tspdf_buffer_append_str(buf, "\nstream\n");
    tspdf_buffer_append(buf, write_data, write_len);
    tspdf_buffer_append_str(buf, "\nendstream");

    if (enc) free(enc);
    free(stream.owned);
    return buf->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

// Encrypted saves pack objects into object streams when the INPUT used them
// (preserve-style, same rule as the plain path): member bodies are serialized
// with plain strings and the whole ObjStm is encrypted as one unit with the
// container's key (ISO 32000 §7.5.7). Classic inputs keep plain top-level
// objects and a classic xref table.
TspdfError tspdf_serialize_encrypted(TspdfReader *doc, TspdfCrypt *crypt,
                                    uint8_t **out_buf, size_t *out_len) {
    if (!doc || !crypt || !out_buf || !out_len) return TSPDF_ERR_PARSE;

    // Pack into fresh object streams when the source stored objects that way;
    // this needs an xref stream (type-2 entries) instead of the classic table.
    bool use_objstm = input_uses_objstm(doc);

    /* Set up a parser for resolving objects */
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *source_catalog = catalog_for_serialize(doc, &parser);

    /* 1. Collect all reachable objects from all pages */
    size_t total_objs = doc->xref.count + doc->new_objs.count;
    bool *visited = (bool *)calloc(total_objs, sizeof(bool));
    if (!visited) return TSPDF_ERR_ALLOC;

    /* Fast path: if document is unmodified, include all in-use xref entries */
    if (!doc->modified && doc->new_objs.count == 0) {
        for (size_t i = 1; i < doc->xref.count; i++) {
            if (doc->xref.entries[i].in_use) {
                visited[i] = true;
            }
        }
    } else {
        for (size_t i = 0; i < doc->pages.count; i++) {
            TspdfReaderPage *page = &doc->pages.pages[i];
            if (page->obj_num < total_objs) {
                visited[page->obj_num] = true;
            }
            collect_from_page_obj(doc, page->page_dict, visited, &parser);
        }
        collect_from_catalog_entries(doc, source_catalog, visited, &parser, false);
    }

    /* The source /Encrypt dict is re-emitted as a dedicated object below
     * (either preserved verbatim or freshly generated); never copy the
     * original as a normal — and then string-encrypted — object. Check the
     * SOURCE document's crypt, not `crypt`: when re-encrypting with new
     * passwords, `crypt` is a fresh encryption crypt (src_encrypt_num 0)
     * while the old dict is recorded in doc->crypt, and copying it would
     * embed the old (offline-crackable) O/U hashes as an orphan object. */
    if (doc->crypt && doc->crypt->src_encrypt_num > 0 &&
        doc->crypt->src_encrypt_num < total_objs) {
        visited[doc->crypt->src_encrypt_num] = false;
    }

    /* 2. Build renumbering map */
    RenumberMap map;
    map.map_size = total_objs;
    map.old_to_new = (uint32_t *)calloc(map.map_size, sizeof(uint32_t));
    if (!map.old_to_new) {
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    uint32_t *collected = NULL;
    size_t collected_count = 0;
    {
        // Skip source ObjStm/XRef containers here for the same reason as the
        // plain path: the fast-collect loop above marks them in-use, but
        // their members are written individually (or re-packed) below.
        for (size_t i = 1; i < total_objs; i++) {
            if (!visited[i]) continue;
            if (is_xref_machinery(resolve_for_collect(doc, (uint32_t)i, &parser))) continue;
            collected_count++;
        }
        collected = (uint32_t *)malloc(sizeof(uint32_t) * (collected_count > 0 ? collected_count : 1));
        if (!collected) {
            free(map.old_to_new);
            free(visited);
            return TSPDF_ERR_ALLOC;
        }
        size_t ci = 0;
        for (size_t i = 1; i < total_objs; i++) {
            if (!visited[i]) continue;
            if (is_xref_machinery(resolve_for_collect(doc, (uint32_t)i, &parser))) continue;
            collected[ci++] = (uint32_t)i;
        }
    }

    /* New obj 1 = Catalog, obj 2 = Pages, then collected objects, then encrypt dict */
    uint32_t next_num = 3;
    for (size_t i = 0; i < collected_count; i++) {
        map.old_to_new[collected[i]] = next_num++;
    }

    uint32_t encrypt_obj_num = next_num++; /* The /Encrypt dict object */

    /* Info dict object (if metadata was modified) */
    bool write_info = metadata_has_changes(doc->metadata);
    uint32_t info_obj_num = 0;
    if (write_info) {
        info_obj_num = next_num++;
    }

    uint32_t total_objects = next_num - 1;

    /* Determine PDF version (xref streams need 1.5+) */
    char version[8];
    memcpy(version, doc->pdf_version, sizeof(version));
    if (use_objstm) {
        if (strcmp(version, "1.0") == 0 || strcmp(version, "1.1") == 0 ||
            strcmp(version, "1.2") == 0 || strcmp(version, "1.3") == 0 ||
            strcmp(version, "1.4") == 0) {
            memcpy(version, "1.5", 4);
        }
    }

    /* 3. Write PDF */
    TspdfBuffer buf = tspdf_buffer_create(doc->data_len > 0 ? doc->data_len : 65536);
    if (buf.error) {
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    /* Header */
    tspdf_buffer_printf(&buf, "%%PDF-%s\n", version);
    tspdf_buffer_append_byte(&buf, '%');
    tspdf_buffer_append_byte(&buf, 0xE2);
    tspdf_buffer_append_byte(&buf, 0xE3);
    tspdf_buffer_append_byte(&buf, 0xCF);
    tspdf_buffer_append_byte(&buf, 0xD3);
    tspdf_buffer_append_byte(&buf, '\n');

    /* Track offsets for the xref, plus type-2 locations when packing */
    uint32_t max_objstms = use_objstm
        ? total_objects / TSPDF_OBJSTM_CAPACITY + 2 : 0;
    size_t table_size = (size_t)total_objects + max_objstms + 2;
    size_t *offsets = (size_t *)calloc(table_size, sizeof(size_t));
    uint32_t *objstm_of = use_objstm ? (uint32_t *)calloc(table_size, sizeof(uint32_t)) : NULL;
    uint32_t *objstm_idx = use_objstm ? (uint32_t *)calloc(table_size, sizeof(uint32_t)) : NULL;
    TspdfBuffer scratch = tspdf_buffer_create(4096);
    ObjStmWriter stm_writer;
    bool stm_writer_ok = objstm_writer_init(&stm_writer, &buf, offsets, objstm_of,
                                            objstm_idx, total_objects + 1, use_objstm,
                                            crypt, false);
    if (!offsets || scratch.error || !stm_writer_ok ||
        (use_objstm && (!objstm_of || !objstm_idx))) {
        objstm_writer_destroy(&stm_writer);
        tspdf_buffer_destroy(&scratch);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    TspdfError emit_err = TSPDF_OK;

    /* Object 1: Catalog. Packed members carry plain strings (the container
     * stream is encrypted whole); top-level objects encrypt per string. */
    if (use_objstm) {
        tspdf_buffer_reset(&scratch);
        write_catalog_body(&scratch, source_catalog, &map, doc, false);
        emit_err = emit_nonstream_object(&stm_writer, 1, &scratch);
    } else {
        offsets[1] = buf.len;
        write_catalog_obj_encrypted(&buf, source_catalog, &map, doc, crypt);
    }

    /* Object 2: Pages (no strings, so both modes share one body) */
    if (emit_err == TSPDF_OK) {
        tspdf_buffer_reset(&scratch);
        tspdf_buffer_append_str(&scratch, "<< /Type /Pages /Kids [ ");
        for (size_t i = 0; i < doc->pages.count; i++) {
            uint32_t old_num = doc->pages.pages[i].obj_num;
            uint32_t new_num = map.old_to_new[old_num];
            if (i > 0) tspdf_buffer_append_byte(&scratch, ' ');
            tspdf_buffer_printf(&scratch, "%u 0 R", new_num);
        }
        tspdf_buffer_printf(&scratch, " ] /Count %zu >>", doc->pages.count);
        emit_err = emit_nonstream_object(&stm_writer, 2, &scratch);
    }
    if (emit_err != TSPDF_OK) {
        objstm_writer_destroy(&stm_writer);
        tspdf_buffer_destroy(&scratch);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return emit_err;
    }

    /* Objects 3..N: the collected objects */
    for (size_t i = 0; i < collected_count; i++) {
        uint32_t old_num = collected[i];
        uint32_t new_num = map.old_to_new[old_num];
        TspdfObj *obj = resolve_for_collect(doc, old_num, &parser);
        if (!obj) continue;

        if (obj->type != TSPDF_OBJ_STREAM && use_objstm) {
            /* Packed member: plain strings, container encrypted at flush. */
            tspdf_buffer_reset(&scratch);
            write_nonstream_body_plain(&scratch, doc, obj, old_num, &map);
            emit_err = emit_nonstream_object(&stm_writer, new_num, &scratch);
            if (emit_err != TSPDF_OK) break;
            continue;
        }

        offsets[new_num] = buf.len;
        tspdf_buffer_printf(&buf, "%u 0 obj\n", new_num);

        if (obj->type == TSPDF_OBJ_STREAM) {
            emit_err = write_stream_obj_encrypted(
                &buf, obj, &map, doc, crypt, old_num, source_object_gen(doc, old_num),
                new_num, 0);
            if (emit_err != TSPDF_OK) break;
        } else if (obj->type == TSPDF_OBJ_DICT) {
            bool is_page = is_serialized_page_object(doc, old_num, obj);
            if (is_page) {
                bool wrote_type = false;
                bool wrote_parent = false;
                tspdf_buffer_append_str(&buf, "<< ");
                for (size_t j = 0; j < obj->dict.count; j++) {
                    const char *key = obj->dict.entries[j].key;
                    write_name(&buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(&buf, ' ');
                    if (strcmp(key, "Type") == 0) {
                        wrote_type = true;
                        write_obj_encrypted(&buf, obj->dict.entries[j].value, &map, doc, crypt, new_num, 0);
                    } else if (strcmp(key, "Parent") == 0) {
                        wrote_parent = true;
                        tspdf_buffer_append_str(&buf, "2 0 R");
                    } else {
                        write_obj_encrypted(&buf, obj->dict.entries[j].value, &map, doc, crypt, new_num, 0);
                    }
                    tspdf_buffer_append_byte(&buf, ' ');
                }
                if (!wrote_type) {
                    tspdf_buffer_append_str(&buf, "/Type /Page ");
                }
                if (!wrote_parent) {
                    tspdf_buffer_append_str(&buf, "/Parent 2 0 R ");
                }
                tspdf_buffer_append_str(&buf, ">>");
            } else {
                write_obj_encrypted(&buf, obj, &map, doc, crypt, new_num, 0);
            }
        } else {
            write_obj_encrypted(&buf, obj, &map, doc, crypt, new_num, 0);
        }

        tspdf_buffer_append_str(&buf, "\nendobj\n");
    }

    if (emit_err != TSPDF_OK) {
        objstm_writer_destroy(&stm_writer);
        tspdf_buffer_destroy(&scratch);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return emit_err;
    }

    /* Encrypt dict object — strings here are NOT encrypted */
    offsets[encrypt_obj_num] = buf.len;
    tspdf_buffer_printf(&buf, "%u 0 obj\n", encrypt_obj_num);

    if (crypt->src_encrypt_dict) {
        /* Preserving the source encryption: copy the original /Encrypt dict
         * verbatim. Together with the original first /ID string (written in
         * the trailer below) and the recovered file key used for the object
         * encryption above, this keeps both original passwords working. */
        write_encrypt_dict_plain(&buf, crypt->src_encrypt_dict, doc, &parser, 0);
    } else if (crypt->version == 4) {
        tspdf_buffer_append_str(&buf, "<< /Filter /Standard /V 4 /R 4 /Length 128");
        tspdf_buffer_append_str(&buf, " /CF << /StdCF << /CFM /AESV2 /Length 16 /AuthEvent /DocOpen >> >>");
        tspdf_buffer_append_str(&buf, " /StmF /StdCF /StrF /StdCF");
        tspdf_buffer_append_str(&buf, " /O ");
        write_hex_string(&buf, crypt->O, crypt->O_len);
        tspdf_buffer_append_str(&buf, " /U ");
        write_hex_string(&buf, crypt->U, crypt->U_len);
        tspdf_buffer_printf(&buf, " /P %d", (int32_t)crypt->permissions);
        tspdf_buffer_append_str(&buf, " >>");
    } else {
        /* V=5, R=6 */
        tspdf_buffer_printf(&buf, "<< /Filter /Standard /V 5 /R %d /Length 256", crypt->revision);
        tspdf_buffer_append_str(&buf, " /CF << /StdCF << /CFM /AESV3 /Length 32 /AuthEvent /DocOpen >> >>");
        tspdf_buffer_append_str(&buf, " /StmF /StdCF /StrF /StdCF");
        tspdf_buffer_append_str(&buf, " /O ");
        write_hex_string(&buf, crypt->O, crypt->O_len);
        tspdf_buffer_append_str(&buf, " /U ");
        write_hex_string(&buf, crypt->U, crypt->U_len);
        tspdf_buffer_append_str(&buf, " /OE ");
        write_hex_string(&buf, crypt->OE, 32);
        tspdf_buffer_append_str(&buf, " /UE ");
        write_hex_string(&buf, crypt->UE, 32);
        tspdf_buffer_append_str(&buf, " /Perms ");
        write_hex_string(&buf, crypt->Perms, 16);
        tspdf_buffer_printf(&buf, " /P %d", (int32_t)crypt->permissions);
        tspdf_buffer_append_str(&buf, " >>");
    }
    tspdf_buffer_append_str(&buf, "\nendobj\n");

    /* Info dict object (if metadata was modified). Its strings ARE encrypted:
     * ISO 32000 §7.6.2 exempts only the /Encrypt dictionary and /ID, so Info
     * values must be encrypted with this object's key like any other string. */
    if (write_info) {
        offsets[info_obj_num] = buf.len;
        write_info_dict_obj(&buf, doc, &parser, info_obj_num, crypt);
    }

    /* Flush the last (partial) object stream */
    emit_err = objstm_writer_flush(&stm_writer);
    uint32_t total_with_objstms = use_objstm ? stm_writer.next_objstm_num - 1
                                             : total_objects;
    objstm_writer_destroy(&stm_writer);
    tspdf_buffer_destroy(&scratch);
    if (emit_err != TSPDF_OK) {
        tspdf_buffer_destroy(&buf);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return emit_err;
    }

    if (use_objstm) {
        /* Type-2 entries need an xref stream; it carries the trailer keys
         * (/Encrypt, /ID) and is itself never encrypted. */
        TspdfError xref_err = write_xref_stream(&buf, offsets, total_with_objstms, 1,
                                                 info_obj_num, write_info, NULL,
                                                 objstm_of, objstm_idx, encrypt_obj_num,
                                                 crypt->file_id, crypt->file_id_len);
        if (xref_err != TSPDF_OK) {
            tspdf_buffer_destroy(&buf);
            free(objstm_idx);
            free(objstm_of);
            free(offsets);
            free(collected);
            free(map.old_to_new);
            free(visited);
            return xref_err;
        }
    } else {
        /* Classic xref table */
        size_t xref_offset = buf.len;
        tspdf_buffer_printf(&buf, "xref\n0 %u\n", total_objects + 1);
        tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
        for (uint32_t i = 1; i <= total_objects; i++) {
            if (offsets[i] > 0) {
                tspdf_buffer_printf(&buf, "%010zu 00000 n \n", offsets[i]);
            } else {
                /* Never emit an in-use (type 1) entry with offset 0: an object
                 * that could not be written becomes a free entry instead. */
                tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
            }
        }

        /* Trailer with /Encrypt and /ID */
        if (write_info) {
            tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root 1 0 R /Encrypt %u 0 R /Info %u 0 R /ID [ ",
                        total_objects + 1, encrypt_obj_num, info_obj_num);
        } else {
            tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root 1 0 R /Encrypt %u 0 R /ID [ ",
                        total_objects + 1, encrypt_obj_num);
        }
        write_hex_string(&buf, crypt->file_id, crypt->file_id_len);
        tspdf_buffer_append_byte(&buf, ' ');
        write_hex_string(&buf, crypt->file_id, crypt->file_id_len);
        tspdf_buffer_append_str(&buf, " ] >>\n");
        tspdf_buffer_printf(&buf, "startxref\n%zu\n%%%%EOF\n", xref_offset);
    }

    if (buf.error) {
        tspdf_buffer_destroy(&buf);
        free(objstm_idx);
        free(objstm_of);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    *out_buf = buf.data;
    *out_len = buf.len;

    free(objstm_idx);
    free(objstm_of);
    free(offsets);
    free(collected);
    free(map.old_to_new);
    free(visited);

    return TSPDF_OK;
}
