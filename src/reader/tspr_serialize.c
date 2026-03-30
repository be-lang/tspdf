#include "tspr_internal.h"
#include "../util/buffer.h"
#include "../compress/deflate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

// --- Forward declarations for write helpers (defined later) ---
static void write_string_escaped(TspdfBuffer *buf, const uint8_t *data, size_t len);
static void write_name(TspdfBuffer *buf, const uint8_t *data, size_t len);

// --- Metadata helpers ---

// Check if metadata has any set flags
static bool metadata_has_changes(const TspdfReaderMetadata *m) {
    if (!m) return false;
    return m->title_set || m->author_set || m->subject_set ||
           m->keywords_set || m->creator_set || m->producer_set;
}

// Build and write an Info dict object at current buffer position.
// Merges original Info dict fields with overrides from doc->metadata.
// Returns the offset where the object was written (already appended to buf).
static void write_info_dict_obj(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                                 uint32_t info_obj_num) {
    // Resolve original Info dict if present
    TspdfObj *orig_info = NULL;
    if (doc->xref.trailer) {
        TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
        if (info_ref) {
            if (info_ref->type == TSPDF_OBJ_REF) {
                uint32_t num = info_ref->ref.num;
                if (num < doc->xref.count) {
                    orig_info = tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache, NULL);
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
                    write_string_escaped(buf, (const uint8_t *)override_val, strlen(override_val));
                    tspdf_buffer_append_byte(buf, ' ');
                }
            } else {
                // Keep original value (skip refs, copy simple values)
                if (val && val->type == TSPDF_OBJ_STRING) {
                    write_name(buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(buf, ' ');
                    write_string_escaped(buf, val->string.data, val->string.len);
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
            write_string_escaped(buf, (const uint8_t *)new_val, strlen(new_val));
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
        write_string_escaped(buf, (const uint8_t *)mod_date, strlen(mod_date));
        tspdf_buffer_append_byte(buf, ' ');
    }

    // Always add /ModDate
    write_name(buf, (const uint8_t *)"ModDate", 7);
    tspdf_buffer_append_byte(buf, ' ');
    write_string_escaped(buf, (const uint8_t *)mod_date, strlen(mod_date));
    tspdf_buffer_append_str(buf, " >>\nendobj\n");
}

// --- Object collection: find all objects reachable from pages ---

static void collect_from_obj(TspdfReader *doc, TspdfObj *obj, bool *visited, TspdfParser *parser);

static TspdfObj *resolve_for_collect(TspdfReader *doc, uint32_t num, TspdfParser *parser) {
    if (num < doc->xref.count) {
        return tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache, NULL);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
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

// --- Helper: write stream with optional recompression ---
static void write_stream_obj_with_options(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                           TspdfReader *doc, const TspdfSaveOptions *opts) {
    TspdfObj *dict = obj->stream.dict;

    // Determine stream bytes source and length
    const uint8_t *stream_data;
    size_t stream_len;
    if (obj->stream.self_contained && obj->stream.data != NULL) {
        stream_data = obj->stream.data;
        stream_len = obj->stream.len;
    } else {
        stream_data = doc->data + obj->stream.raw_offset;
        stream_len = obj->stream.raw_len;
    }

    // Recompression
    uint8_t *recompressed = NULL;
    size_t recomp_len = 0;
    if (opts->recompress_streams) {
        TspdfObj *filter = tspdf_dict_get(dict, "Filter");
        if (filter && filter->type == TSPDF_OBJ_NAME &&
            strcmp((char *)filter->string.data, "FlateDecode") == 0) {
            size_t dec_len = 0;
            uint8_t *decompressed = deflate_decompress(stream_data, stream_len, &dec_len);
            if (decompressed) {
                recompressed = deflate_compress(decompressed, dec_len, &recomp_len);
                free(decompressed);
                if (recompressed && recomp_len < stream_len) {
                    stream_data = recompressed;
                    stream_len = recomp_len;
                } else {
                    free(recompressed);
                    recompressed = NULL;
                }
            }
        }
    }

    // Write dict with /Length overridden
    tspdf_buffer_append_str(buf, "<< ");
    for (size_t i = 0; i < dict->dict.count; i++) {
        const char *key = dict->dict.entries[i].key;
        write_name(buf, (const uint8_t *)key, strlen(key));
        tspdf_buffer_append_byte(buf, ' ');
        if (strcmp(key, "Length") == 0) {
            tspdf_buffer_printf(buf, "%zu", stream_len);
        } else {
            write_obj(buf, dict->dict.entries[i].value, map, doc);
        }
        tspdf_buffer_append_byte(buf, ' ');
    }
    tspdf_buffer_append_str(buf, ">>");

    tspdf_buffer_append_str(buf, "\nstream\n");
    tspdf_buffer_append(buf, stream_data, stream_len);
    tspdf_buffer_append_str(buf, "\nendstream");

    free(recompressed);
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

// --- Helper: write xref stream ---
static void write_xref_stream(TspdfBuffer *buf, size_t *offsets, uint32_t total_objects,
                                uint32_t root_ref, uint32_t info_obj_num,
                                bool write_info, const TspdfSaveOptions *opts) {
    // The xref stream is written as a new object (total_objects + 1)
    uint32_t xref_obj_num = total_objects + 1;
    size_t xref_stream_offset = buf->len;
    uint32_t size_val = xref_obj_num + 1;  // /TspdfSize is highest obj num + 1

    // Build raw xref entry data: W = [1 4 2], 7 bytes per entry
    // Entry 0 (free): type=0 offset=0 gen=65535
    // Entries 1..total_objects (in-use): type=1 offset=N gen=0
    size_t entry_count = (size_t)total_objects + 1;  // 0..total_objects
    size_t raw_len = entry_count * 7;
    uint8_t *raw = (uint8_t *)calloc(raw_len, 1);
    if (!raw) return;

    // Entry 0: free
    raw[0] = 0;  // type = free
    raw[1] = 0; raw[2] = 0; raw[3] = 0; raw[4] = 0;  // offset = 0
    raw[5] = 0xFF; raw[6] = 0xFF;  // gen = 65535

    for (uint32_t i = 1; i <= total_objects; i++) {
        size_t base = (size_t)i * 7;
        raw[base] = 1;  // type = in-use
        uint32_t off = (uint32_t)offsets[i];
        raw[base + 1] = (uint8_t)(off >> 24);
        raw[base + 2] = (uint8_t)(off >> 16);
        raw[base + 3] = (uint8_t)(off >> 8);
        raw[base + 4] = (uint8_t)(off);
        raw[base + 5] = 0;  // gen high
        raw[base + 6] = 0;  // gen low
    }

    // Compress
    size_t comp_len = 0;
    uint8_t *compressed = deflate_compress(raw, raw_len, &comp_len);
    free(raw);
    if (!compressed) return;

    // Write xref stream object
    tspdf_buffer_printf(buf, "%u 0 obj\n<< /Type /XRef /TspdfSize %u /W [ 1 4 2 ] /Root %u 0 R",
                  xref_obj_num, size_val, root_ref);
    if (write_info && !opts->strip_metadata) {
        tspdf_buffer_printf(buf, " /Info %u 0 R", info_obj_num);
    }
    tspdf_buffer_printf(buf, " /Length %zu /Filter /FlateDecode >>\nstream\n", comp_len);
    tspdf_buffer_append(buf, compressed, comp_len);
    tspdf_buffer_append_str(buf, "\nendstream\nendobj\n");
    free(compressed);

    tspdf_buffer_printf(buf, "startxref\n%zu\n%%%%EOF\n", xref_stream_offset);
}

TspdfError tspdf_serialize_with_options(TspdfReader *doc, uint8_t **out_buf, size_t *out_len,
                                      const TspdfSaveOptions *opts) {
    if (!doc || !out_buf || !out_len || !opts) return TSPDF_ERR_PARSE;

    // --- Preserve object IDs fast path ---
    // When preserve_object_ids is set and document is unmodified, copy raw bytes
    bool use_preserve = opts->preserve_object_ids && !doc->modified && doc->data != NULL
                        && doc->new_objs.count == 0;

    // Set up a parser for resolving objects
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    // 1. Collect all reachable objects
    size_t total_objs = doc->xref.count + doc->new_objs.count;
    bool *visited = (bool *)calloc(total_objs, sizeof(bool));
    if (!visited) return TSPDF_ERR_ALLOC;

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
            collect_from_obj(doc, page->page_dict, visited, &parser);
        }
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
        if (opts->use_xref_stream) {
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
            tspdf_buffer_append_str(&buf, "/Producer (tspr) >>\nendobj\n");
        }

        // Write xref / trailer
        if (opts->use_xref_stream) {
            uint32_t root_for_trailer = orig_root_num;
            uint32_t info_for_trailer = (!opts->strip_metadata && need_producer_info) ? producer_info_num : 0;
            write_xref_stream(&buf, offsets, max_obj, root_for_trailer,
                               info_for_trailer, info_for_trailer > 0, opts);
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

            tspdf_buffer_printf(&buf, "trailer\n<< /TspdfSize %u /Root %u 0 R",
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
            if (visited[i]) collected_count++;
        }
        collected = (uint32_t *)malloc(sizeof(uint32_t) * (collected_count > 0 ? collected_count : 1));
        if (!collected) {
            free(map.old_to_new);
            free(visited);
            return TSPDF_ERR_ALLOC;
        }
        size_t ci = 0;
        for (size_t i = 1; i < total_objs; i++) {
            if (visited[i]) collected[ci++] = (uint32_t)i;
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
    if (opts->use_xref_stream) {
        if (strcmp(version, "1.0") == 0 || strcmp(version, "1.1") == 0 ||
            strcmp(version, "1.2") == 0 || strcmp(version, "1.3") == 0 ||
            strcmp(version, "1.4") == 0) {
            memcpy(version, "1.5", 4);
        }
    }

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

    // Track offsets for xref
    size_t *offsets = (size_t *)calloc(total_objects + 1, sizeof(size_t));
    if (!offsets) {
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    // Object 1: Catalog
    offsets[1] = buf.len;
    tspdf_buffer_append_str(&buf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    // Object 2: Pages
    offsets[2] = buf.len;
    tspdf_buffer_append_str(&buf, "2 0 obj\n<< /Type /Pages /Kids [ ");
    for (size_t i = 0; i < doc->pages.count; i++) {
        uint32_t old_num = doc->pages.pages[i].obj_num;
        uint32_t new_num = map.old_to_new[old_num];
        if (i > 0) tspdf_buffer_append_byte(&buf, ' ');
        tspdf_buffer_printf(&buf, "%u 0 R", new_num);
    }
    tspdf_buffer_printf(&buf, " ] /Count %zu >>\nendobj\n", doc->pages.count);

    // Objects 3..total_objects: the collected objects
    for (size_t i = 0; i < collected_count; i++) {
        uint32_t old_num = collected[i];
        uint32_t new_num = map.old_to_new[old_num];
        TspdfObj *obj = resolve_for_collect(doc, old_num, &parser);
        if (!obj) continue;

        // Skip metadata objects if strip_metadata
        if (opts->strip_metadata && is_xmp_metadata(obj)) continue;

        offsets[new_num] = buf.len;
        tspdf_buffer_printf(&buf, "%u 0 obj\n", new_num);

        if (obj->type == TSPDF_OBJ_STREAM) {
            write_stream_obj_with_options(&buf, obj, &map, doc, opts);
        } else if (obj->type == TSPDF_OBJ_DICT) {
            TspdfObj *type_val = tspdf_dict_get(obj, "Type");
            bool is_page = (type_val && type_val->type == TSPDF_OBJ_NAME &&
                           strcmp((const char *)type_val->string.data, "Page") == 0);
            if (is_page) {
                tspdf_buffer_append_str(&buf, "<< ");
                for (size_t j = 0; j < obj->dict.count; j++) {
                    const char *key = obj->dict.entries[j].key;
                    write_name(&buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(&buf, ' ');
                    if (strcmp(key, "Parent") == 0) {
                        tspdf_buffer_append_str(&buf, "2 0 R");
                    } else {
                        write_obj(&buf, obj->dict.entries[j].value, &map, doc);
                    }
                    tspdf_buffer_append_byte(&buf, ' ');
                }
                tspdf_buffer_append_str(&buf, ">>");
            } else {
                write_obj(&buf, obj, &map, doc);
            }
        } else {
            write_obj(&buf, obj, &map, doc);
        }

        tspdf_buffer_append_str(&buf, "\nendobj\n");
    }

    // Info dict object
    if (write_info) {
        offsets[info_obj_num] = buf.len;
        if (metadata_has_changes(doc->metadata)) {
            write_info_dict_obj(&buf, doc, &parser, info_obj_num);
        } else if (opts->update_producer) {
            // Just write a simple Info dict with /Producer
            tspdf_buffer_printf(&buf, "%u 0 obj\n<< /Producer (tspr) >>\nendobj\n", info_obj_num);
        }
    }

    // Xref table or xref stream
    if (opts->use_xref_stream) {
        write_xref_stream(&buf, offsets, total_objects, 1, info_obj_num, write_info, opts);
    } else {
        size_t xref_offset = buf.len;
        tspdf_buffer_printf(&buf, "xref\n0 %u\n", total_objects + 1);
        tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
        for (uint32_t i = 1; i <= total_objects; i++) {
            tspdf_buffer_printf(&buf, "%010zu 00000 n \n", offsets[i]);
        }

        if (write_info) {
            tspdf_buffer_printf(&buf, "trailer\n<< /TspdfSize %u /Root 1 0 R /Info %u 0 R >>\n",
                          total_objects + 1, info_obj_num);
        } else {
            tspdf_buffer_printf(&buf, "trailer\n<< /TspdfSize %u /Root 1 0 R >>\n", total_objects + 1);
        }
        tspdf_buffer_printf(&buf, "startxref\n%zu\n%%%%EOF\n", xref_offset);
    }

    if (buf.error) {
        tspdf_buffer_destroy(&buf);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    *out_buf = buf.data;
    *out_len = buf.len;

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

static void write_stream_obj_encrypted(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                        TspdfReader *doc, TspdfCrypt *crypt,
                                        uint32_t cur_obj_num, uint16_t cur_gen) {
    TspdfObj *dict = obj->stream.dict;

    /* Get stream raw data */
    const uint8_t *stream_data;
    size_t stream_len;
    if (obj->stream.self_contained && obj->stream.data != NULL) {
        stream_data = obj->stream.data;
        stream_len = obj->stream.len;
    } else {
        stream_data = doc->data + obj->stream.raw_offset;
        stream_len = obj->stream.raw_len;
    }

    /* Encrypt the stream data */
    size_t enc_len = 0;
    uint8_t *enc = tspdf_crypt_encrypt_stream(crypt, cur_obj_num, cur_gen,
                                              stream_data, stream_len, &enc_len);
    const uint8_t *write_data = enc ? enc : stream_data;
    size_t write_len = enc ? enc_len : stream_len;

    /* Write dict with /Length overridden and strings encrypted */
    tspdf_buffer_append_str(buf, "<< ");
    for (size_t i = 0; i < dict->dict.count; i++) {
        const char *key = dict->dict.entries[i].key;
        write_name(buf, (const uint8_t *)key, strlen(key));
        tspdf_buffer_append_byte(buf, ' ');
        if (strcmp(key, "Length") == 0) {
            tspdf_buffer_printf(buf, "%zu", write_len);
        } else {
            write_obj_encrypted(buf, dict->dict.entries[i].value, map, doc, crypt, cur_obj_num, cur_gen);
        }
        tspdf_buffer_append_byte(buf, ' ');
    }
    tspdf_buffer_append_str(buf, ">>");

    tspdf_buffer_append_str(buf, "\nstream\n");
    tspdf_buffer_append(buf, write_data, write_len);
    tspdf_buffer_append_str(buf, "\nendstream");

    if (enc) free(enc);
}

TspdfError tspdf_serialize_encrypted(TspdfReader *doc, TspdfCrypt *crypt,
                                    uint8_t **out_buf, size_t *out_len) {
    if (!doc || !crypt || !out_buf || !out_len) return TSPDF_ERR_PARSE;

    /* Set up a parser for resolving objects */
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

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
            collect_from_obj(doc, page->page_dict, visited, &parser);
        }
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
        for (size_t i = 1; i < total_objs; i++) {
            if (visited[i]) collected_count++;
        }
        collected = (uint32_t *)malloc(sizeof(uint32_t) * (collected_count > 0 ? collected_count : 1));
        if (!collected) {
            free(map.old_to_new);
            free(visited);
            return TSPDF_ERR_ALLOC;
        }
        size_t ci = 0;
        for (size_t i = 1; i < total_objs; i++) {
            if (visited[i]) collected[ci++] = (uint32_t)i;
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

    /* 3. Write PDF */
    TspdfBuffer buf = tspdf_buffer_create(doc->data_len > 0 ? doc->data_len : 65536);
    if (buf.error) {
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    /* Header */
    tspdf_buffer_printf(&buf, "%%PDF-%s\n", doc->pdf_version);
    tspdf_buffer_append_byte(&buf, '%');
    tspdf_buffer_append_byte(&buf, 0xE2);
    tspdf_buffer_append_byte(&buf, 0xE3);
    tspdf_buffer_append_byte(&buf, 0xCF);
    tspdf_buffer_append_byte(&buf, 0xD3);
    tspdf_buffer_append_byte(&buf, '\n');

    /* Track offsets for xref */
    size_t *offsets = (size_t *)calloc(total_objects + 1, sizeof(size_t));
    if (!offsets) {
        tspdf_buffer_destroy(&buf);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    /* Object 1: Catalog */
    offsets[1] = buf.len;
    tspdf_buffer_append_str(&buf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    /* Object 2: Pages */
    offsets[2] = buf.len;
    tspdf_buffer_append_str(&buf, "2 0 obj\n<< /Type /Pages /Kids [ ");
    for (size_t i = 0; i < doc->pages.count; i++) {
        uint32_t old_num = doc->pages.pages[i].obj_num;
        uint32_t new_num = map.old_to_new[old_num];
        if (i > 0) tspdf_buffer_append_byte(&buf, ' ');
        tspdf_buffer_printf(&buf, "%u 0 R", new_num);
    }
    tspdf_buffer_printf(&buf, " ] /Count %zu >>\nendobj\n", doc->pages.count);

    /* Objects 3..N: the collected objects (encrypted) */
    for (size_t i = 0; i < collected_count; i++) {
        uint32_t old_num = collected[i];
        uint32_t new_num = map.old_to_new[old_num];
        TspdfObj *obj = resolve_for_collect(doc, old_num, &parser);
        if (!obj) continue;

        offsets[new_num] = buf.len;
        tspdf_buffer_printf(&buf, "%u 0 obj\n", new_num);

        if (obj->type == TSPDF_OBJ_STREAM) {
            write_stream_obj_encrypted(&buf, obj, &map, doc, crypt, new_num, 0);
        } else if (obj->type == TSPDF_OBJ_DICT) {
            TspdfObj *type_val = tspdf_dict_get(obj, "Type");
            bool is_page = (type_val && type_val->type == TSPDF_OBJ_NAME &&
                           strcmp((const char *)type_val->string.data, "Page") == 0);
            if (is_page) {
                tspdf_buffer_append_str(&buf, "<< ");
                for (size_t j = 0; j < obj->dict.count; j++) {
                    const char *key = obj->dict.entries[j].key;
                    write_name(&buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(&buf, ' ');
                    if (strcmp(key, "Parent") == 0) {
                        tspdf_buffer_append_str(&buf, "2 0 R");
                    } else {
                        write_obj_encrypted(&buf, obj->dict.entries[j].value, &map, doc, crypt, new_num, 0);
                    }
                    tspdf_buffer_append_byte(&buf, ' ');
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

    /* Encrypt dict object — strings here are NOT encrypted */
    offsets[encrypt_obj_num] = buf.len;
    tspdf_buffer_printf(&buf, "%u 0 obj\n", encrypt_obj_num);

    if (crypt->version == 4) {
        tspdf_buffer_append_str(&buf, "<< /Type /Crypt /Filter /Standard /V 4 /R 4 /Length 128");
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

    /* Info dict object (if metadata was modified) — strings NOT encrypted per PDF spec */
    if (write_info) {
        offsets[info_obj_num] = buf.len;
        write_info_dict_obj(&buf, doc, &parser, info_obj_num);
    }

    /* Xref table */
    size_t xref_offset = buf.len;
    tspdf_buffer_printf(&buf, "xref\n0 %u\n", total_objects + 1);
    tspdf_buffer_append_str(&buf, "0000000000 65535 f \n");
    for (uint32_t i = 1; i <= total_objects; i++) {
        tspdf_buffer_printf(&buf, "%010zu 00000 n \n", offsets[i]);
    }

    /* Trailer with /Encrypt and /ID */
    if (write_info) {
        tspdf_buffer_printf(&buf, "trailer\n<< /TspdfSize %u /Root 1 0 R /Encrypt %u 0 R /Info %u 0 R /ID [ ",
                      total_objects + 1, encrypt_obj_num, info_obj_num);
    } else {
        tspdf_buffer_printf(&buf, "trailer\n<< /TspdfSize %u /Root 1 0 R /Encrypt %u 0 R /ID [ ",
                      total_objects + 1, encrypt_obj_num);
    }
    write_hex_string(&buf, crypt->file_id, crypt->file_id_len);
    tspdf_buffer_append_byte(&buf, ' ');
    write_hex_string(&buf, crypt->file_id, crypt->file_id_len);
    tspdf_buffer_append_str(&buf, " ] >>\n");
    tspdf_buffer_printf(&buf, "startxref\n%zu\n%%%%EOF\n", xref_offset);

    if (buf.error) {
        tspdf_buffer_destroy(&buf);
        free(offsets);
        free(collected);
        free(map.old_to_new);
        free(visited);
        return TSPDF_ERR_ALLOC;
    }

    *out_buf = buf.data;
    *out_len = buf.len;

    free(offsets);
    free(collected);
    free(map.old_to_new);
    free(visited);

    return TSPDF_OK;
}
