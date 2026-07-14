#include "tspr_internal.h"
#include "../pdf/primitives.h"
#include "../util/buffer.h"
#include "../compress/deflate.h"
#include "../crypto/md5.h"
#include "../../include/tspdf/version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

// --- Forward declarations for write helpers (defined later) ---
static void write_hex_string(TspdfBuffer *buf, const uint8_t *data, size_t len);
static void write_trailer_id_array(TspdfBuffer *buf, const uint8_t *file_id,
                                   size_t file_id_len);
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
bool tspr_is_xref_machinery(TspdfObj *obj) {
    return tspr_stream_dict_type_is(obj, "ObjStm") ||
           tspr_stream_dict_type_is(obj, "XRef");
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
// RenumberMap is defined in tspr_internal.h (shared with the Info-plan module).

// Context for one indirect object's body emission: the renumber map, the
// owning document, the crypt adapter the strings go through, and the object's
// own number/generation (per-object key derivation for encrypted saves).
typedef struct {
    const RenumberMap *map;
    TspdfReader *doc;
    const TspdfCryptAdapter *ad;
    uint32_t obj_num;
    uint16_t gen;
} ObjWriteCtx;

static void write_obj_ctx(TspdfBuffer *buf, TspdfObj *obj, const ObjWriteCtx *ctx);
static void write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc);

// --- Identity crypt adapter: plain saves ---

static void identity_write_string(const TspdfCryptAdapter *ad, TspdfBuffer *buf,
                                  const uint8_t *data, size_t len,
                                  uint32_t obj_num, uint16_t gen) {
    (void)ad; (void)obj_num; (void)gen;
    tspdf_pdf_encode_string(buf, data, len);
}

static uint8_t *identity_transform_stream(const TspdfCryptAdapter *ad,
                                          TspdfObj *stream_obj, uint32_t obj_num,
                                          uint16_t gen, const uint8_t *data,
                                          size_t len, size_t *out_len) {
    (void)ad; (void)stream_obj; (void)obj_num; (void)gen; (void)data; (void)len;
    (void)out_len;
    return NULL;  // write the input unchanged
}

TspdfCryptAdapter tspr_crypt_adapter_identity(void) {
    TspdfCryptAdapter ad = {0};
    ad.write_string = identity_write_string;
    ad.transform_stream = identity_transform_stream;
    // No /Encrypt object, no trailer /ID: emit hooks stay NULL.
    return ad;
}

// Exported thin wrapper so tspr_infoplan.c can call the canonical encoder
// without duplicating it (see tspr_internal.h).
void tspr_write_string_escaped(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_pdf_encode_string(buf, data, len);
}

static void write_name(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_pdf_encode_name(buf, data, len);
}

void tspr_write_name(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    tspdf_pdf_encode_name(buf, data, len);
}

void tspr_write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc) {
    write_obj(buf, obj, map, doc);
}

// Identity-adapter convenience wrapper: plain object body under a renumber
// map. Callers that emit through a crypt adapter build an ObjWriteCtx.
static void write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc) {
    TspdfCryptAdapter identity = tspr_crypt_adapter_identity();
    ObjWriteCtx ctx = { map, doc, &identity, 0, 0 };
    write_obj_ctx(buf, obj, &ctx);
}

static void write_obj_ctx(TspdfBuffer *buf, TspdfObj *obj, const ObjWriteCtx *ctx) {
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
            // The crypt seam: escaped literal (identity) or encrypted hex.
            ctx->ad->write_string(ctx->ad, buf, obj->string.data,
                                  obj->string.len, ctx->obj_num, ctx->gen);
            break;
        case TSPDF_OBJ_NAME:
            write_name(buf, obj->string.data, obj->string.len);
            break;
        case TSPDF_OBJ_ARRAY:
            tspdf_buffer_append_str(buf, "[ ");
            for (size_t i = 0; i < obj->array.count; i++) {
                if (i > 0) tspdf_buffer_append_byte(buf, ' ');
                write_obj_ctx(buf, &obj->array.items[i], ctx);
            }
            tspdf_buffer_append_str(buf, " ]");
            break;
        case TSPDF_OBJ_DICT:
            tspdf_buffer_append_str(buf, "<< ");
            for (size_t i = 0; i < obj->dict.count; i++) {
                write_name(buf, (const uint8_t *)obj->dict.entries[i].key,
                           strlen(obj->dict.entries[i].key));
                tspdf_buffer_append_byte(buf, ' ');
                write_obj_ctx(buf, obj->dict.entries[i].value, ctx);
                tspdf_buffer_append_byte(buf, ' ');
            }
            tspdf_buffer_append_str(buf, ">>");
            break;
        case TSPDF_OBJ_STREAM:
            // Should not be reached directly - streams are handled at top level
            break;
        case TSPDF_OBJ_REF: {
            uint32_t new_num = 0;
            if (obj->ref.num < ctx->map->map_size) {
                new_num = ctx->map->old_to_new[obj->ref.num];
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

// Serialize a non-stream object body (no "N 0 obj" wrapper): page dicts get
// /Parent forced to the synthetic pages object (2 0 R) and /Type /Page
// ensured; everything else is written verbatim through the renumber map.
// Strings go through ctx->ad — the identity adapter for plain saves AND for
// members of an encrypted object stream (ISO 32000 §7.5.7: the container is
// encrypted as one unit, member strings never individually), the crypt
// adapter for encrypted top-level objects (ctx->obj_num is their key).
static void write_nonstream_body(TspdfBuffer *scratch, TspdfObj *obj,
                                 uint32_t old_num, const ObjWriteCtx *ctx) {
    if (obj->type == TSPDF_OBJ_DICT && is_serialized_page_object(ctx->doc, old_num, obj)) {
        bool wrote_type = false;
        bool wrote_parent = false;
        tspdf_buffer_append_str(scratch, "<< ");
        for (size_t j = 0; j < obj->dict.count; j++) {
            const char *key = obj->dict.entries[j].key;
            write_name(scratch, (const uint8_t *)key, strlen(key));
            tspdf_buffer_append_byte(scratch, ' ');
            if (strcmp(key, "Type") == 0) {
                wrote_type = true;
                write_obj_ctx(scratch, obj->dict.entries[j].value, ctx);
            } else if (strcmp(key, "Parent") == 0) {
                wrote_parent = true;
                tspdf_buffer_append_str(scratch, "2 0 R");
            } else {
                write_obj_ctx(scratch, obj->dict.entries[j].value, ctx);
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
        write_obj_ctx(scratch, obj, ctx);
    }
}

// Writes the catalog dict body only ("<< ... >>", no obj wrapper) so the
// caller can place it either top-level or inside an object stream. ctx is
// the emission context for object 1 (strings through its adapter).
static void write_catalog_body(TspdfBuffer *buf, TspdfObj *catalog,
                               const ObjWriteCtx *ctx, bool strip_metadata) {
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
            write_obj_ctx(buf, catalog->dict.entries[i].value, ctx);
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

bool tspr_stream_dict_type_is(TspdfObj *obj, const char *type_name) {
    if (!obj || obj->type != TSPDF_OBJ_STREAM || !type_name) {
        return false;
    }
    TspdfObj *type_val = tspdf_dict_get(obj->stream.dict, "Type");
    return type_val && type_val->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)type_val->string.data, type_name) == 0;
}

// Thin static alias so existing call sites in this file stay unchanged.
static bool stream_dict_type_is(TspdfObj *obj, const char *type_name) {
    return tspr_stream_dict_type_is(obj, type_name);
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
// `recompress` is the effective recompression decision (never true for
// encrypted saves); `new_num` is the object's number in the OUTPUT file —
// the crypt adapter derives the payload/string key from it.
static TspdfError write_stream_obj_with_options(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map,
                                                TspdfReader *doc, bool recompress,
                                                const TspdfCryptAdapter *ad, uint32_t new_num,
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

    // Recompression. The strategy is chosen by tspdf_stream_plan from the dict
    // alone (which handles the XRef skip, the filter classification, the XMP
    // and external-file guards, and the indirect-/DecodeParms armor refusal);
    // this executor runs the deflate for the chosen strategy and keeps whichever
    // encoding is smaller, so a stream never grows. The size-focused best-effort
    // deflate level is used throughout except for image XObjects (fast level);
    // `tspdf compress` is the only recompress caller.
    uint8_t *recompressed = NULL;
    size_t recomp_len = 0;
    bool add_flate_filter = false;    // append /Filter /FlateDecode
    bool replace_filter = false;      // drop original /Filter + /DecodeParms too
    TspdfStreamPlan plan = tspdf_stream_plan(
        obj, recompress, stream_dict_type_is(obj, "XRef"), stream_len);
    switch (plan.action) {
        case TSPDF_STREAM_KEEP:
            break;
        case TSPDF_STREAM_REDEFLATE: {
            // Flate stream (predictor params, if any, are preserved since the
            // re-encoded bytes are the same predicted bytes): inflate +
            // re-deflate and keep the smaller encoding. Recovers streams
            // stored at a low compression level (or as raw stored blocks).
            size_t dec_len = 0;
            uint8_t *decompressed = deflate_decompress(stream_data, stream_len, &dec_len);
            if (decompressed) {
                recompressed = plan.use_fast_level
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
            break;
        }
        case TSPDF_STREAM_ADD_FLATE:
            // Stream stored with no filter at all: deflate it and add the
            // /Filter entry if that shrinks it.
            recompressed = deflate_compress_best(stream_data, stream_len, &recomp_len);
            if (recompressed && recomp_len < stream_len) {
                stream_data = recompressed;
                stream_len = recomp_len;
                add_flate_filter = true;
            } else {
                free(recompressed);
                recompressed = NULL;
            }
            break;
        case TSPDF_STREAM_ARMOR_STRIP: {
            // ASCII/RunLength/LZW armor (possibly stacked on Flate): decode the
            // chain fully and re-encode as plain Flate when smaller, replacing
            // the whole /Filter (+/DecodeParms). A failed decode keeps the
            // stream verbatim.
            uint8_t *decoded = NULL;
            size_t dec_len = 0;
            if (tspdf_stream_decode(dict, stream_data, stream_len,
                                    &decoded, &dec_len) == TSPDF_OK) {
                recompressed = plan.use_fast_level
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
            break;
        }
    }

    // The crypt seam: encrypt the (possibly recompressed) payload with this
    // object's key. NULL = identity (or the /EncryptMetadata false exemption).
    size_t enc_len = 0;
    uint8_t *encrypted = ad->transform_stream(ad, obj, new_num, 0,
                                              stream_data, stream_len, &enc_len);
    if (encrypted) {
        stream_data = encrypted;
        stream_len = enc_len;
    }

    // Write dict with /Length overridden; string values go through the
    // adapter with this object's key.
    ObjWriteCtx ctx = { map, doc, ad, new_num, 0 };
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
            write_obj_ctx(buf, dict->dict.entries[i].value, &ctx);
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

    free(encrypted);
    free(recompressed);
    free(stream.owned);
    return buf->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

// --- Helper: check if object is XMP metadata ---
bool tspr_is_xmp_metadata(TspdfObj *obj) {
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

static bool is_xmp_metadata(TspdfObj *obj) {
    return tspr_is_xmp_metadata(obj);
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
// encrypt_obj_num != 0 adds the /Encrypt and /ID trailer keys (the adapter
// emits the /ID array); the xref stream itself is never encrypted (ISO 32000
// §7.5.8.2 — readers must parse it before any key is available), so writing
// it here with plain deflate is correct even when the rest of the file is
// encrypted.
static TspdfError write_xref_stream(TspdfBuffer *buf, size_t *offsets, uint32_t total_objects,
                                      uint32_t root_ref, uint32_t info_obj_num,
                                      bool write_info,
                                      const uint32_t *objstm_of, const uint32_t *objstm_idx,
                                      uint32_t encrypt_obj_num,
                                      const TspdfCryptAdapter *ad) {
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
    if (write_info) {
        tspdf_buffer_printf(buf, " /Info %u 0 R", info_obj_num);
    }
    if (encrypt_obj_num != 0) {
        tspdf_buffer_printf(buf, " /Encrypt %u 0 R /ID [ ", encrypt_obj_num);
        ad->emit_trailer_id(ad, buf);
        tspdf_buffer_append_str(buf, " ]");
    }
    tspdf_buffer_printf(buf, " /Length %zu /Filter /FlateDecode >>\nstream\n", comp_len);
    tspdf_buffer_append(buf, compressed, comp_len);
    tspdf_buffer_append_str(buf, "\nendstream\nendobj\n");
    free(compressed);

    tspdf_buffer_printf(buf, "startxref\n%zu\n%%%%EOF\n", xref_stream_offset);
    return buf->error ? TSPDF_ERR_ALLOC : TSPDF_OK;
}

// --- Save-shape decisions: the ONE place plain and encrypted saves differ ---
//
// The serializer below is shared; encryption is an emission seam
// (TspdfCryptAdapter). These decisions are where the two historical paths
// GENUINELY diverged, preserved verbatim from the pre-unification fork:
//
//   decision             plain save                        encrypted save
//   -------------------  --------------------------------  ---------------------
//   recompress streams   opts->recompress_streams          never
//   ObjStm packing       recompress || input used ObjStm   input used ObjStm
//   xref stream          opts->use_xref_stream || packing  packing only
//   strip metadata       opts->strip_metadata              never
//   strip unused         opts->strip_unused_objects        never
//   preserve object ids  opts->preserve_object_ids         never
//   info plan            (opts, encrypted=false)           (NULL, encrypted=true)
//
// Encrypted saves ignore every TspdfSaveOptions field: their entry points
// (a default save of a password-opened doc, tspdf_reader_save_encrypted)
// never carried options, and the historical fork discarded them. Honoring
// them here would silently change the shape of encrypted output.
typedef struct {
    bool input_objstm;     // the source file stored objects in ObjStms
    bool recompress;       // stream recompression + best-effort ObjStm deflate
    bool use_objstm;       // pack eligible non-stream objects into ObjStms
    bool use_xref_stream;  // compressed xref stream instead of classic table
    bool strip_metadata;   // drop Info + XMP
    bool strip_unused;     // reachability walk instead of fast collect
    bool preserve_ids;     // raw-copy fast path eligible
} SaveShape;

static SaveShape decide_save_shape(const TspdfReader *doc,
                                   const TspdfSaveOptions *opts, bool encrypted) {
    SaveShape s = {0};
    // Inputs that used object streams are re-packed on save (preserve-style,
    // qpdf's default): exploding them into classic top-level objects roughly
    // doubles ObjStm-heavy files. Classic inputs are only packed when
    // recompression asks for minimal output.
    s.input_objstm = input_uses_objstm(doc);
    s.recompress = !encrypted && opts->recompress_streams;
    s.use_objstm = s.recompress || s.input_objstm;
    // recompress targets minimal output size, so it also implies the compact
    // compressed xref stream over the classic table (whose 20 bytes per
    // object dominate the achievable savings on object-heavy files); ObjStm
    // packing requires one outright (type-2 entries).
    s.use_xref_stream = (!encrypted && opts->use_xref_stream) || s.use_objstm;
    s.strip_metadata = !encrypted && opts->strip_metadata;
    s.strip_unused = !encrypted && opts->strip_unused_objects;
    s.preserve_ids = !encrypted && opts->preserve_object_ids;
    return s;
}

static TspdfError serialize_common(TspdfReader *doc, TspdfCrypt *crypt,
                                   uint8_t **out_buf, size_t *out_len,
                                   const TspdfSaveOptions *opts);

TspdfError tspdf_serialize_with_options(TspdfReader *doc, uint8_t **out_buf, size_t *out_len,
                                      const TspdfSaveOptions *opts) {
    if (!doc || !out_buf || !out_len || !opts) return TSPDF_ERR_PARSE;

    // A document opened with a password stays encrypted on save: silently
    // writing it decrypted would strip its protection behind the caller's
    // back. The encrypted save reuses the recovered file key and copies the
    // source /Encrypt dict and /ID verbatim, so both original passwords keep
    // working. opts->decrypt is the explicit opt-out (`tspdf decrypt`).
    if (doc->crypt && !opts->decrypt) {
        return tspdf_serialize_encrypted(doc, doc->crypt, out_buf, out_len);
    }
    return serialize_common(doc, NULL, out_buf, out_len, opts);
}

// Encrypted save: the same serializer emitting through the crypt adapter.
// `crypt` is either the document's own (preserved encryption) or a fresh one
// (tspdf_reader_save_to_memory_encrypted re-encrypting with new passwords).
// Save options are deliberately not a parameter (see decide_save_shape).
TspdfError tspdf_serialize_encrypted(TspdfReader *doc, TspdfCrypt *crypt,
                                    uint8_t **out_buf, size_t *out_len) {
    if (!doc || !crypt || !out_buf || !out_len) return TSPDF_ERR_PARSE;
    TspdfSaveOptions opts = tspdf_save_options_default();
    return serialize_common(doc, crypt, out_buf, out_len, &opts);
}

static TspdfError serialize_common(TspdfReader *doc, TspdfCrypt *crypt,
                                   uint8_t **out_buf, size_t *out_len,
                                   const TspdfSaveOptions *opts) {
    bool encrypted = crypt != NULL;
    TspdfCryptAdapter ad = encrypted ? tspr_crypt_adapter_encrypt(crypt)
                                     : tspr_crypt_adapter_identity();
    SaveShape shape = decide_save_shape(doc, opts, encrypted);
    bool use_xref_stream = shape.use_xref_stream;

    // --- Preserve object IDs fast path ---
    // When preserve_object_ids is set and document is unmodified, copy raw
    // bytes. Not possible for object-stream inputs: type-2 entries have no
    // byte offset to copy from, so those go through the standard path below.
    bool use_preserve = shape.preserve_ids && !shape.strip_metadata &&
                        !doc->modified && doc->data != NULL
                        && doc->new_objs.count == 0 && !shape.input_objstm;

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
                            && !shape.strip_unused;

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
                                     shape.strip_metadata);
    }

    // The source /Encrypt dict is re-emitted as a dedicated object on
    // encrypted saves (either preserved verbatim or freshly generated);
    // never copy the original as a normal — and then string-encrypted —
    // object. Check the SOURCE document's crypt, not `crypt`: when
    // re-encrypting with new passwords, `crypt` is a fresh encryption crypt
    // (src_encrypt_num 0) while the old dict is recorded in doc->crypt, and
    // copying it would embed the old (offline-crackable) O/U hashes as an
    // orphan object.
    if (encrypted && doc->crypt && doc->crypt->src_encrypt_num > 0 &&
        doc->crypt->src_encrypt_num < total_objs) {
        visited[doc->crypt->src_encrypt_num] = false;
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

        // Write producer Info dict if needed. The preserve-ids path keeps
        // original object numbers, so the emitter renders source values under
        // an identity map (write_obj), preserving non-string fields verbatim.
        if (need_producer_info) {
            offsets[producer_info_num] = buf.len;
            RenumberMap identity_map;
            identity_map.map_size = total_objs;
            identity_map.old_to_new = (uint32_t *)calloc(total_objs, sizeof(uint32_t));
            if (identity_map.old_to_new) {
                for (size_t k = 0; k < total_objs; k++)
                    identity_map.old_to_new[k] = (uint32_t)k;
                tspdf_info_emit_producer(&buf, doc, &parser, producer_info_num,
                                         &identity_map);
                free(identity_map.old_to_new);
            } else {
                tspdf_buffer_printf(&buf, "%u 0 obj\n<< /Producer (tspdf "
                                    TSPDF_VERSION_STRING ") >>\nendobj\n",
                                    producer_info_num);
            }
        }

        // Write xref / trailer
        if (use_xref_stream) {
            uint32_t root_for_trailer = orig_root_num;
            uint32_t info_for_trailer = (!opts->strip_metadata && need_producer_info) ? producer_info_num : 0;
            TspdfError xref_err = write_xref_stream(&buf, offsets, max_obj, root_for_trailer,
                                                     info_for_trailer, info_for_trailer > 0,
                                                     NULL, NULL, 0, &ad);
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

    // One module decides what happens to /Info (carry / merge / producer /
    // drop). When it says CARRY, the source Info dict is reused and the trailer
    // keeps referencing it; dropping that reference used to throw the
    // document's Title/Author away. The slow-collect walk never reaches Info
    // (it hangs off the trailer, not the page tree), so mark it reachable here.
    // Encrypted saves pass no options (they are ignored; see decide_save_shape)
    // and never stamp Producer alone — `encrypted` selects CARRY over PRODUCER,
    // and a carried Info's strings are re-encrypted with its own key.
    TspdfInfoPlan info_plan = tspdf_info_plan(doc, &parser,
                                              encrypted ? NULL : opts, encrypted);
    bool reference_source_info = info_plan.action == TSPDF_INFO_CARRY_SOURCE;
    if (reference_source_info) {
        visited[source_info_num] = true;
    }

    // 2. Collect the emit list and build the renumbering map. New obj 1 =
    // Catalog (synthetic), obj 2 = Pages (synthetic), so collected objects are
    // numbered from 3. The shared walk drops xref machinery, the source root
    // (emitted as obj 1 here), and — under strip_metadata — the source Info
    // and XMP streams. Difference kept from the historical fork: encrypted
    // saves DO carry the source catalog through collection (it is re-emitted
    // as obj 1 but not dropped from the collected set), so skip_root is false
    // there. That duplicate is the "encrypted-save bloat" backlog item; when
    // it is fixed, this skip_root split is the one place to change.
    TspdfCollectResult collect = {0};
    tspdf_collect_objects(doc, &parser, visited, total_objs, /*base_num=*/3,
                          /*skip_root=*/!encrypted,
                          encrypted ? 0 : source_root_num,
                          shape.strip_metadata, source_info_num, &collect);
    if (!collect.ok) {
        free(visited);
        return TSPDF_ERR_ALLOC;
    }
    RenumberMap map = collect.map;
    uint32_t *collected = collect.collected;
    size_t collected_count = collect.collected_count;

    uint32_t next_num = 3 + (uint32_t)collected_count;

    // The /Encrypt dict object (encrypted saves only), numbered right after
    // the collected objects, before any fresh Info — the fork's layout.
    uint32_t encrypt_obj_num = 0;
    if (encrypted) {
        encrypt_obj_num = next_num++;
    }

    // Determine if we need an Info object
    bool write_info = info_plan.action == TSPDF_INFO_WRITE_MERGED ||
                      info_plan.action == TSPDF_INFO_WRITE_PRODUCER;
    uint32_t info_obj_num = 0;
    if (write_info) {
        info_obj_num = next_num++;
    } else if (reference_source_info) {
        // The carried-over source Info object, at its new number.
        info_obj_num = map.old_to_new[source_info_num];
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

    // Object-stream packing (decided in decide_save_shape): when recompression
    // asks for minimal output, or when the input itself used object streams
    // (unpacking those into classic objects roughly doubles such files). Both
    // cases imply use_xref_stream, which type-2 entries require. Encrypted
    // saves pack too: the writer encrypts each flushed ObjStm as one unit
    // with the container's own object key, so member bodies are serialized
    // through the identity adapter (ISO 32000 §7.5.7).
    bool use_objstm = shape.use_objstm;
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
                                            crypt, shape.recompress);
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

    // Non-stream bodies go through the crypt adapter only when written
    // top-level; ObjStm members stay plain (the flushed container is
    // encrypted as one unit with its own key — never member strings).
    TspdfCryptAdapter identity = tspr_crypt_adapter_identity();
    const TspdfCryptAdapter *body_ad = use_objstm ? &identity : &ad;

    // Object 1: Catalog
    tspdf_buffer_reset(&scratch);
    {
        ObjWriteCtx catalog_ctx = { &map, doc, body_ad, 1, 0 };
        write_catalog_body(&scratch, source_catalog, &catalog_ctx,
                           shape.strip_metadata);
    }
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
        if (shape.strip_metadata && is_xmp_metadata(obj)) continue;

        if (obj->type == TSPDF_OBJ_STREAM) {
            // Streams are never packed into object streams (spec rule);
            // always written top-level, so their payload and dict strings go
            // through the real adapter with the object's own key.
            offsets[new_num] = buf.len;
            tspdf_buffer_printf(&buf, "%u 0 obj\n", new_num);
            emit_err = write_stream_obj_with_options(
                &buf, obj, &map, doc, shape.recompress, &ad, new_num,
                old_num, source_object_gen(doc, old_num));
            tspdf_buffer_append_str(&buf, "\nendobj\n");
            continue;
        }

        tspdf_buffer_reset(&scratch);
        {
            ObjWriteCtx obj_ctx = { &map, doc, body_ad, new_num, 0 };
            write_nonstream_body(&scratch, obj, old_num, &obj_ctx);
        }
        emit_err = emit_nonstream_object(&stm_writer, new_num, &scratch);
    }

    // The /Encrypt dict object. Its strings are never object-key encrypted
    // (the spec's one exemption), so the adapter emits them as raw hex.
    if (emit_err == TSPDF_OK && encrypt_obj_num != 0) {
        offsets[encrypt_obj_num] = buf.len;
        tspdf_buffer_printf(&buf, "%u 0 obj\n", encrypt_obj_num);
        ad.emit_encrypt_dict(&ad, &buf, doc, &parser);
        tspdf_buffer_append_str(&buf, "\nendobj\n");
    }

    // Info dict object. Kept top-level even when packing: the emitter writes a
    // complete indirect object, and one small dict does not change the size
    // math. The plan chose MERGED (metadata edits) or PRODUCER (producer
    // refresh only, plain saves only); the standard path passes no identity
    // map, so the producer emitter copies only string fields as plain
    // literals. A merged Info in an encrypted file has its strings encrypted
    // with its own key (ISO 32000 §7.6.2 exempts only /Encrypt and /ID).
    if (emit_err == TSPDF_OK && write_info) {
        offsets[info_obj_num] = buf.len;
        if (info_plan.action == TSPDF_INFO_WRITE_MERGED) {
            tspdf_info_emit_merged(&buf, doc, &parser, info_obj_num, crypt);
        } else {
            tspdf_info_emit_producer(&buf, doc, &parser, info_obj_num, NULL);
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

    // Xref table or xref stream. The xref stream carries the /Encrypt and
    // /ID trailer keys on encrypted saves and is itself never encrypted.
    if (use_xref_stream) {
        TspdfError xref_err = write_xref_stream(&buf, offsets, total_with_objstms, 1,
                                                 info_obj_num, info_obj_num != 0,
                                                 objstm_of, objstm_idx,
                                                 encrypt_obj_num, &ad);
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

        // Classic trailer; encrypted saves add /Encrypt (before /Info — the
        // fork's key order, kept byte-identical) and the /ID array.
        tspdf_buffer_printf(&buf, "trailer\n<< /Size %u /Root 1 0 R",
                            total_objects + 1);
        if (encrypt_obj_num != 0) {
            tspdf_buffer_printf(&buf, " /Encrypt %u 0 R", encrypt_obj_num);
        }
        if (info_obj_num != 0) {
            tspdf_buffer_printf(&buf, " /Info %u 0 R", info_obj_num);
        }
        if (encrypt_obj_num != 0) {
            tspdf_buffer_append_str(&buf, " /ID [ ");
            ad.emit_trailer_id(&ad, &buf);
            tspdf_buffer_append_str(&buf, " ]");
        }
        tspdf_buffer_append_str(&buf, " >>\n");
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

void tspr_write_hex_string(TspdfBuffer *buf, const uint8_t *data, size_t len) {
    write_hex_string(buf, data, len);
}

// Trailer /ID entries. The first is the crypt's file id — for V<5 handlers it
// is key material (MD5'd into every object key), so preserved re-encrypts
// must keep it byte-identical or the original passwords stop working. The
// second is the "changed when the file is updated" half (ISO 32000 §14.4):
// derive it from the bytes serialized so far, which makes it distinct from
// the first yet deterministic (no time()/rand()), so saves whose content is
// byte-identical stay byte-identical.
static void write_trailer_id_array(TspdfBuffer *buf, const uint8_t *file_id,
                                   size_t file_id_len) {
    uint8_t second[16];
    md5_hash(buf->data, buf->len, second);
    write_hex_string(buf, file_id, file_id_len);
    tspdf_buffer_append_byte(buf, ' ');
    write_hex_string(buf, second, sizeof(second));
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

// --- Crypt adapter: encrypted saves ---
//
// The counterpart to the identity adapter near the top of this file. The
// serializer never touches TspdfCrypt directly during emission; everything
// encryption-shaped goes through these four hooks.

static void crypt_write_string(const TspdfCryptAdapter *ad, TspdfBuffer *buf,
                               const uint8_t *data, size_t len,
                               uint32_t obj_num, uint16_t gen) {
    size_t enc_len = 0;
    uint8_t *enc = tspdf_crypt_encrypt_string(ad->crypt, obj_num, gen,
                                              data, len, &enc_len);
    if (enc) {
        write_hex_string(buf, enc, enc_len);
        free(enc);
    } else {
        // Encryption failure: fall back to the plaintext as hex (historical
        // behavior; a readable value beats a corrupt file).
        write_hex_string(buf, data, len);
    }
}

static uint8_t *crypt_transform_stream(const TspdfCryptAdapter *ad,
                                       TspdfObj *stream_obj, uint32_t obj_num,
                                       uint16_t gen, const uint8_t *data,
                                       size_t len, size_t *out_len) {
    // /EncryptMetadata false: the XMP metadata stream is stored in the clear
    // (ISO 32000 §7.6.3.2); encrypting it anyway would garble it for readers
    // honoring the flag.
    if (!ad->crypt->encrypt_metadata &&
        stream_dict_type_is(stream_obj, "Metadata")) {
        return NULL;
    }
    return tspdf_crypt_encrypt_stream(ad->crypt, obj_num, gen, data, len, out_len);
}

static void crypt_emit_encrypt_dict(const TspdfCryptAdapter *ad, TspdfBuffer *buf,
                                    TspdfReader *doc, TspdfParser *parser) {
    TspdfCrypt *crypt = ad->crypt;

    if (crypt->src_encrypt_dict) {
        /* Preserving the source encryption: copy the original /Encrypt dict
         * verbatim. Together with the original first /ID string and the
         * recovered file key used for the object encryption, this keeps both
         * original passwords working. */
        write_encrypt_dict_plain(buf, crypt->src_encrypt_dict, doc, parser, 0);
    } else if (crypt->version == 4) {
        tspdf_buffer_append_str(buf, "<< /Filter /Standard /V 4 /R 4 /Length 128");
        tspdf_buffer_append_str(buf, " /CF << /StdCF << /CFM /AESV2 /Length 16 /AuthEvent /DocOpen >> >>");
        tspdf_buffer_append_str(buf, " /StmF /StdCF /StrF /StdCF");
        tspdf_buffer_append_str(buf, " /O ");
        write_hex_string(buf, crypt->O, crypt->O_len);
        tspdf_buffer_append_str(buf, " /U ");
        write_hex_string(buf, crypt->U, crypt->U_len);
        tspdf_buffer_printf(buf, " /P %d", (int32_t)crypt->permissions);
        tspdf_buffer_append_str(buf, " >>");
    } else {
        /* V=5, R=6 */
        tspdf_buffer_printf(buf, "<< /Filter /Standard /V 5 /R %d /Length 256", crypt->revision);
        tspdf_buffer_append_str(buf, " /CF << /StdCF << /CFM /AESV3 /Length 32 /AuthEvent /DocOpen >> >>");
        tspdf_buffer_append_str(buf, " /StmF /StdCF /StrF /StdCF");
        tspdf_buffer_append_str(buf, " /O ");
        write_hex_string(buf, crypt->O, crypt->O_len);
        tspdf_buffer_append_str(buf, " /U ");
        write_hex_string(buf, crypt->U, crypt->U_len);
        tspdf_buffer_append_str(buf, " /OE ");
        write_hex_string(buf, crypt->OE, 32);
        tspdf_buffer_append_str(buf, " /UE ");
        write_hex_string(buf, crypt->UE, 32);
        tspdf_buffer_append_str(buf, " /Perms ");
        write_hex_string(buf, crypt->Perms, 16);
        tspdf_buffer_printf(buf, " /P %d", (int32_t)crypt->permissions);
        tspdf_buffer_append_str(buf, " >>");
    }
}

static void crypt_emit_trailer_id(const TspdfCryptAdapter *ad, TspdfBuffer *buf) {
    write_trailer_id_array(buf, ad->crypt->file_id, ad->crypt->file_id_len);
}

TspdfCryptAdapter tspr_crypt_adapter_encrypt(TspdfCrypt *crypt) {
    TspdfCryptAdapter ad = {0};
    ad.crypt = crypt;
    ad.write_string = crypt_write_string;
    ad.transform_stream = crypt_transform_stream;
    ad.emit_encrypt_dict = crypt_emit_encrypt_dict;
    ad.emit_trailer_id = crypt_emit_trailer_id;
    return ad;
}
