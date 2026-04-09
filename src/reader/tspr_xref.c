#include "tspr_internal.h"
#include "../compress/deflate.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// --- Helpers ---

// Find the last startxref in the tail of the file (last 4096 bytes).
static bool find_startxref(const uint8_t *data, size_t len, size_t *out_offset) {
    const char *needle = "startxref";
    size_t needle_len = 9;

    if (len < needle_len) {
        return false;
    }

    // Search the last 4096 bytes (enough for linearized PDFs with two startxref markers)
    size_t search_start = (len > 4096) ? len - 4096 : 0;

    for (size_t i = len - needle_len; i >= search_start; i--) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            // Parse the offset that follows
            size_t pos = i + needle_len;
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                                  data[pos] == '\r' || data[pos] == '\n')) {
                pos++;
            }
            size_t num_start = pos;
            while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
                pos++;
            }
            if (pos == num_start) {
                if (i == 0) {
                    break;
                }
                continue;
            }

            char buf[32];
            size_t nlen = pos - num_start;
            if (nlen >= sizeof(buf)) {
                return false;
            }
            memcpy(buf, data + num_start, nlen);
            buf[nlen] = '\0';
            *out_offset = (size_t)strtoull(buf, NULL, 10);
            return true;
        }
        if (i == 0) {
            break;
        }
    }
    return false;
}

static bool ensure_xref_capacity(TspdfReaderXref *xref, size_t needed, TspdfArena *arena) {
    if (needed <= xref->count) return true;
    TspdfReaderXrefEntry *new_entries = (TspdfReaderXrefEntry *)tspdf_arena_alloc_zero(arena, sizeof(TspdfReaderXrefEntry) * needed);
    if (!new_entries) return false;
    if (xref->entries && xref->count > 0) {
        memcpy(new_entries, xref->entries, sizeof(TspdfReaderXrefEntry) * xref->count);
    }
    xref->entries = new_entries;
    xref->count = needed;
    return true;
}

// Parse a classic xref table at the current parser position.
// p->pos should point at "xref"
// *out_trailer receives the trailer dict for this specific xref section.
static TspdfError parse_classic_xref(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **out_trailer) {
    // Skip "xref"
    if (p->pos + 4 > p->len || memcmp(p->data + p->pos, "xref", 4) != 0) {
        return TSPDF_ERR_XREF;
    }
    p->pos += 4;
    tspdf_skip_whitespace(p);

    // Parse subsections
    while (p->pos < p->len) {
        // Check if we hit "trailer"
        if (p->pos + 7 <= p->len && memcmp(p->data + p->pos, "trailer", 7) == 0) {
            break;
        }

        // Parse first_obj count
        size_t start = p->pos;
        while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
        if (p->pos == start) return TSPDF_ERR_XREF;
        char buf[32];
        size_t nlen = p->pos - start;
        if (nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
        memcpy(buf, p->data + start, nlen);
        buf[nlen] = '\0';
        size_t first_obj = (size_t)strtoull(buf, NULL, 10);

        tspdf_skip_whitespace(p);

        start = p->pos;
        while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
        if (p->pos == start) return TSPDF_ERR_XREF;
        nlen = p->pos - start;
        if (nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
        memcpy(buf, p->data + start, nlen);
        buf[nlen] = '\0';
        size_t entry_count = (size_t)strtoull(buf, NULL, 10);

        // Skip to next line
        while (p->pos < p->len && (p->data[p->pos] == ' ' || p->data[p->pos] == '\t')) {
            p->pos++;
        }
        if (p->pos < p->len && p->data[p->pos] == '\r') p->pos++;
        if (p->pos < p->len && p->data[p->pos] == '\n') p->pos++;

        // Ensure capacity
        size_t needed = first_obj + entry_count;
        if (!ensure_xref_capacity(xref, needed, p->arena)) {
            return TSPDF_ERR_ALLOC;
        }

        // Parse entries (20-byte lines: "OOOOOOOOOO GGGGG n \n" or "f \n")
        for (size_t i = 0; i < entry_count; i++) {
            // Each entry is exactly 20 bytes: 10 offset + ' ' + 5 gen + ' ' + type + ' ' + eol
            // But we parse more flexibly
            tspdf_skip_whitespace(p);

            // Parse offset (10 digits)
            start = p->pos;
            while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
            nlen = p->pos - start;
            if (nlen == 0 || nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
            memcpy(buf, p->data + start, nlen);
            buf[nlen] = '\0';
            size_t offset = (size_t)strtoull(buf, NULL, 10);

            // Skip space
            while (p->pos < p->len && p->data[p->pos] == ' ') p->pos++;

            // Parse gen (5 digits)
            start = p->pos;
            while (p->pos < p->len && isdigit(p->data[p->pos])) p->pos++;
            nlen = p->pos - start;
            if (nlen == 0 || nlen >= sizeof(buf)) return TSPDF_ERR_XREF;
            memcpy(buf, p->data + start, nlen);
            buf[nlen] = '\0';
            uint16_t gen = (uint16_t)strtoul(buf, NULL, 10);

            // Skip space
            while (p->pos < p->len && p->data[p->pos] == ' ') p->pos++;

            // Parse type: 'n' or 'f'
            if (p->pos >= p->len) return TSPDF_ERR_XREF;
            uint8_t type_ch = p->data[p->pos++];

            size_t idx = first_obj + i;
            if (!xref->entries[idx].seen) {
                xref->entries[idx].offset = offset;
                xref->entries[idx].gen = gen;
                xref->entries[idx].in_use = (type_ch == 'n');
                xref->entries[idx].compressed = false;
                xref->entries[idx].seen = true;
            }

            // Skip rest of line (space + eol chars)
            while (p->pos < p->len && (p->data[p->pos] == ' ' ||
                                         p->data[p->pos] == '\r' ||
                                         p->data[p->pos] == '\n')) {
                p->pos++;
            }
        }
    }

    // Parse trailer
    if (p->pos + 7 > p->len || memcmp(p->data + p->pos, "trailer", 7) != 0) {
        return TSPDF_ERR_XREF;
    }
    p->pos += 7;
    tspdf_skip_whitespace(p);

    TspdfObj *trailer = tspdf_parse_obj(p);
    if (!trailer || trailer->type != TSPDF_OBJ_DICT) {
        return TSPDF_ERR_XREF;
    }

    // Only set main trailer if we don't already have one (newer xref takes priority)
    if (!xref->trailer) {
        xref->trailer = trailer;
    }

    *out_trailer = trailer;
    return TSPDF_OK;
}

// Parse xref stream (PDF 1.5+)
static TspdfError parse_xref_stream(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **out_trailer) {
    uint32_t num;
    uint16_t gen;
    TspdfObj *obj = tspdf_parse_indirect_obj(p, &num, &gen);
    if (!obj || obj->type != TSPDF_OBJ_STREAM) {
        return TSPDF_ERR_XREF;
    }

    TspdfObj *stream_dict = obj->stream.dict;

    // Check for /Type /XRef
    TspdfObj *type_val = tspdf_dict_get(stream_dict, "Type");
    if (!type_val || type_val->type != TSPDF_OBJ_NAME ||
        strcmp((const char *)type_val->string.data, "XRef") != 0) {
        return TSPDF_ERR_XREF;
    }

    // Get /W array (field widths)
    TspdfObj *w_obj = tspdf_dict_get(stream_dict, "W");
    if (!w_obj || w_obj->type != TSPDF_OBJ_ARRAY || w_obj->array.count != 3) {
        return TSPDF_ERR_XREF;
    }
    int w[3];
    for (int i = 0; i < 3; i++) {
        if (w_obj->array.items[i].type != TSPDF_OBJ_INT) return TSPDF_ERR_XREF;
        w[i] = (int)w_obj->array.items[i].integer;
    }
    int entry_size = w[0] + w[1] + w[2];
    if (entry_size <= 0) return TSPDF_ERR_XREF;

    // Get /Size
    TspdfObj *size_obj = tspdf_dict_get(stream_dict, "Size");
    if (!size_obj || size_obj->type != TSPDF_OBJ_INT) return TSPDF_ERR_XREF;
    size_t total_objects = (size_t)size_obj->integer;

    // Decompress stream
    // Check for /Filter
    TspdfObj *filter = tspdf_dict_get(stream_dict, "Filter");
    uint8_t *decompressed = NULL;
    size_t decompressed_len = 0;

    const uint8_t *raw_data = p->data + obj->stream.raw_offset;
    size_t raw_len = obj->stream.raw_len;

    if (filter && filter->type == TSPDF_OBJ_NAME &&
        strcmp((const char *)filter->string.data, "FlateDecode") == 0) {
        decompressed = deflate_decompress(raw_data, raw_len, &decompressed_len);
        if (!decompressed) return TSPDF_ERR_XREF;
    } else if (!filter) {
        // No compression
        decompressed = (uint8_t *)raw_data;
        decompressed_len = raw_len;
    } else {
        return TSPDF_ERR_UNSUPPORTED;
    }

    // Get /Index array (default: [0 TspdfSize])
    size_t *subsections = NULL;
    size_t num_subsections = 0;
    TspdfObj *index_obj = tspdf_dict_get(stream_dict, "Index");
    if (index_obj && index_obj->type == TSPDF_OBJ_ARRAY) {
        num_subsections = index_obj->array.count / 2;
        subsections = (size_t *)tspdf_arena_alloc(p->arena, sizeof(size_t) * index_obj->array.count);
        if (!subsections) {
            if (filter) free(decompressed);
            return TSPDF_ERR_ALLOC;
        }
        for (size_t i = 0; i < index_obj->array.count; i++) {
            subsections[i] = (size_t)index_obj->array.items[i].integer;
        }
    } else {
        num_subsections = 1;
        subsections = (size_t *)tspdf_arena_alloc(p->arena, sizeof(size_t) * 2);
        if (!subsections) {
            if (filter) free(decompressed);
            return TSPDF_ERR_ALLOC;
        }
        subsections[0] = 0;
        subsections[1] = total_objects;
    }

    // Ensure capacity
    if (!ensure_xref_capacity(xref, total_objects, p->arena)) {
        if (filter) free(decompressed);
        return TSPDF_ERR_ALLOC;
    }

    // Unpack entries
    size_t data_pos = 0;
    for (size_t s = 0; s < num_subsections; s++) {
        size_t first = subsections[s * 2];
        size_t count = subsections[s * 2 + 1];

        for (size_t i = 0; i < count; i++) {
            if (data_pos + (size_t)entry_size > decompressed_len) {
                if (filter) free(decompressed);
                return TSPDF_ERR_XREF;
            }

            // Read fields
            uint64_t fields[3] = {0, 0, 0};
            for (int f = 0; f < 3; f++) {
                for (int b = 0; b < w[f]; b++) {
                    fields[f] = (fields[f] << 8) | decompressed[data_pos++];
                }
            }

            // Default: if w[0] == 0, type defaults to 1
            uint64_t type = (w[0] == 0) ? 1 : fields[0];
            size_t idx = first + i;
            if (idx >= xref->count) continue;

            if (!xref->entries[idx].seen) {
                xref->entries[idx].seen = true;
                switch (type) {
                    case 0: // free
                        xref->entries[idx].in_use = false;
                        xref->entries[idx].offset = (size_t)fields[1];
                        xref->entries[idx].gen = (uint16_t)fields[2];
                        break;
                    case 1: // normal (uncompressed)
                        xref->entries[idx].in_use = true;
                        xref->entries[idx].compressed = false;
                        xref->entries[idx].offset = (size_t)fields[1];
                        xref->entries[idx].gen = (uint16_t)fields[2];
                        break;
                    case 2: // compressed in object stream
                        xref->entries[idx].in_use = true;
                        xref->entries[idx].compressed = true;
                        xref->entries[idx].stream_obj = (uint32_t)fields[1];
                        xref->entries[idx].stream_idx = (uint16_t)fields[2];
                        break;
                }
            }
        }
    }

    if (filter) free(decompressed);

    // The stream dict itself serves as the trailer
    if (!xref->trailer) {
        xref->trailer = stream_dict;
    }

    *out_trailer = stream_dict;
    return TSPDF_OK;
}

static TspdfError parse_xref_from_offset(TspdfParser *p, TspdfReaderXref *xref, size_t xref_offset) {
    // Follow /Prev chain from the given offset
    while (1) {
        if (xref_offset >= p->len) return TSPDF_ERR_XREF;

        p->pos = xref_offset;
        tspdf_skip_whitespace(p);

        TspdfObj *current_trailer = NULL;
        TspdfError err;
        if (p->pos + 4 <= p->len && memcmp(p->data + p->pos, "xref", 4) == 0) {
            err = parse_classic_xref(p, xref, &current_trailer);
        } else {
            // Might be an xref stream (starts with object number)
            err = parse_xref_stream(p, xref, &current_trailer);
        }

        if (err != TSPDF_OK) return err;

        // Check for /Prev (incremental updates)
        TspdfObj *prev = tspdf_dict_get(current_trailer, "Prev");
        if (prev && prev->type == TSPDF_OBJ_INT) {
            xref_offset = (size_t)prev->integer;
        } else {
            break;
        }
    }

    return TSPDF_OK;
}

TspdfError tspdf_xref_parse(TspdfParser *p, TspdfReaderXref *xref) {
    size_t xref_offset;
    if (!find_startxref(p->data, p->len, &xref_offset)) {
        return TSPDF_ERR_XREF;
    }

    return parse_xref_from_offset(p, xref, xref_offset);
}

// Recursively decrypt all strings in an object tree
static void decrypt_obj_strings(TspdfCrypt *crypt, TspdfObj *obj, uint32_t obj_num, uint16_t gen) {
    if (!obj) return;
    switch (obj->type) {
        case TSPDF_OBJ_STRING:
            tspdf_crypt_decrypt_string(crypt, obj_num, gen, obj->string.data, &obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++)
                decrypt_obj_strings(crypt, &obj->array.items[i], obj_num, gen);
            break;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++)
                decrypt_obj_strings(crypt, obj->dict.entries[i].value, obj_num, gen);
            break;
        case TSPDF_OBJ_STREAM:
            // Decrypt strings in the stream dict (but not stream data here)
            decrypt_obj_strings(crypt, obj->stream.dict, obj_num, gen);
            break;
        default:
            break;
    }
}

// Check if an object is the /Encrypt dict (should not be decrypted)
static bool is_encrypt_dict_obj(TspdfReaderXref *xref, uint32_t obj_num) {
    if (!xref->trailer) return false;
    TspdfObj *encrypt = tspdf_dict_get(xref->trailer, "Encrypt");
    if (!encrypt) return false;
    if (encrypt->type == TSPDF_OBJ_REF) return encrypt->ref.num == obj_num;
    return false;
}

TspdfObj *tspdf_xref_resolve(TspdfReaderXref *xref, TspdfParser *p, uint32_t obj_num, TspdfObj **cache, TspdfCrypt *crypt) {
    if (obj_num >= xref->count) return NULL;
    if (!xref->entries[obj_num].in_use) return NULL;

    // Check cache
    if (cache[obj_num]) return cache[obj_num];

    TspdfReaderXrefEntry *entry = &xref->entries[obj_num];

    if (!entry->compressed) {
        // Normal object: seek to offset and parse
        size_t saved_pos = p->pos;
        p->pos = entry->offset;

        uint32_t num;
        uint16_t gen;
        TspdfObj *obj = tspdf_parse_indirect_obj(p, &num, &gen);
        p->pos = saved_pos;

        if (!obj) return NULL;

        // Decrypt strings if encryption is active
        if (crypt && !is_encrypt_dict_obj(xref, obj_num)) {
            // Skip /Type /XRef objects
            bool is_xref = false;
            TspdfObj *check_dict = (obj->type == TSPDF_OBJ_STREAM) ? obj->stream.dict :
                                  (obj->type == TSPDF_OBJ_DICT) ? obj : NULL;
            if (check_dict) {
                TspdfObj *type_val = tspdf_dict_get(check_dict, "Type");
                if (type_val && type_val->type == TSPDF_OBJ_NAME &&
                    strcmp((const char *)type_val->string.data, "XRef") == 0) {
                    is_xref = true;
                }
            }
            if (!is_xref) {
                decrypt_obj_strings(crypt, obj, obj_num, entry->gen);
            }
        }

        cache[obj_num] = obj;
        return obj;
    } else {
        // Compressed object inside an object stream
        // First resolve the object stream itself
        TspdfObj *stream_obj = tspdf_xref_resolve(xref, p, entry->stream_obj, cache, crypt);
        if (!stream_obj || stream_obj->type != TSPDF_OBJ_STREAM) return NULL;

        // Decompress the stream if not already done
        if (!stream_obj->stream.data) {
            const uint8_t *raw = p->data + stream_obj->stream.raw_offset;
            size_t raw_len = stream_obj->stream.raw_len;

            // For encrypted PDFs, decrypt the raw stream data before decompression
            uint8_t *decrypted_raw = NULL;
            size_t decrypted_len = raw_len;
            if (crypt) {
                TspdfReaderXrefEntry *stream_entry = &xref->entries[entry->stream_obj];
                decrypted_raw = tspdf_crypt_decrypt_stream(crypt, entry->stream_obj,
                                                          stream_entry->gen, raw, raw_len,
                                                          &decrypted_len);
                if (!decrypted_raw) return NULL;
                raw = decrypted_raw;
                raw_len = decrypted_len;
            }

            TspdfObj *filter = tspdf_dict_get(stream_obj->stream.dict, "Filter");
            if (filter && filter->type == TSPDF_OBJ_NAME &&
                strcmp((const char *)filter->string.data, "FlateDecode") == 0) {
                stream_obj->stream.data = deflate_decompress(raw, raw_len, &stream_obj->stream.len);
                free(decrypted_raw);
                if (!stream_obj->stream.data) return NULL;
            } else if (!filter) {
                if (decrypted_raw) {
                    // Use the already-allocated decrypted buffer
                    stream_obj->stream.data = decrypted_raw;
                    stream_obj->stream.len = decrypted_len;
                } else {
                    // Allocate a copy so we can always free later consistently
                    stream_obj->stream.data = (uint8_t *)malloc(raw_len);
                    if (!stream_obj->stream.data) return NULL;
                    memcpy(stream_obj->stream.data, raw, raw_len);
                    stream_obj->stream.len = raw_len;
                }
            } else {
                free(decrypted_raw);
                return NULL;
            }
        }

        // Get /First and /N from the object stream dict
        TspdfObj *first_obj = tspdf_dict_get(stream_obj->stream.dict, "First");
        TspdfObj *n_obj = tspdf_dict_get(stream_obj->stream.dict, "N");
        if (!first_obj || first_obj->type != TSPDF_OBJ_INT) return NULL;
        if (!n_obj || n_obj->type != TSPDF_OBJ_INT) return NULL;

        size_t first_offset = (size_t)first_obj->integer;
        size_t n = (size_t)n_obj->integer;

        if (entry->stream_idx >= n) return NULL;

        // Parse the offset table from the beginning of the decompressed data
        // Format: pairs of (obj_num offset) repeated N times
        // Set up a temporary parser on the decompressed data
        TspdfParser sp;
        tspdf_parser_init(&sp, stream_obj->stream.data, stream_obj->stream.len, p->arena);

        // Read offset table entries
        size_t target_offset = 0;
        for (size_t i = 0; i <= (size_t)entry->stream_idx; i++) {
            tspdf_skip_whitespace(&sp);
            // Parse object number (skip it)
            TspdfObj *on = tspdf_parse_obj(&sp);
            if (!on || on->type != TSPDF_OBJ_INT) return NULL;

            tspdf_skip_whitespace(&sp);
            // Parse relative offset
            TspdfObj *off = tspdf_parse_obj(&sp);
            if (!off || off->type != TSPDF_OBJ_INT) return NULL;

            if (i == (size_t)entry->stream_idx) {
                target_offset = (size_t)off->integer;
            }
        }

        // Seek to the object data (first_offset + relative offset)
        sp.pos = first_offset + target_offset;
        TspdfObj *result = tspdf_parse_obj(&sp);
        if (!result) return NULL;

        cache[obj_num] = result;
        return result;
    }
}
