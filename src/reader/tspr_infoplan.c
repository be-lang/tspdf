// One module decides what happens to the trailer /Info dictionary on save and
// emits the resulting object. See tspr_internal.h for the interface and the
// history (three bugs came from this logic being spread across the plain and
// encrypted save paths).

#include "tspr_internal.h"
#include "../util/buffer.h"
#include "../util/pdftext.h"
#include "../../include/tspdf/version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// --- Metadata edit detection ---

bool tspdf_metadata_has_changes(const TspdfReaderMetadata *m) {
    if (!m) return false;
    return m->title_set || m->author_set || m->subject_set ||
           m->keywords_set || m->creator_set || m->producer_set;
}

// Resolve the source document's Info dict (trailer /Info), NULL when absent
// or not a dict. Resolves with the document's own crypt so values from an
// encrypted source arrive decrypted.
static TspdfObj *source_info_dict(TspdfReader *doc, TspdfParser *parser) {
    if (!doc->xref.trailer) return NULL;
    TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
    if (!info_ref) return NULL;
    if (info_ref->type == TSPDF_OBJ_REF) {
        if (info_ref->ref.num >= doc->xref.count) return NULL;
        TspdfObj *o = tspdf_xref_resolve(&doc->xref, parser, info_ref->ref.num,
                                         doc->obj_cache, doc->crypt);
        return o && o->type == TSPDF_OBJ_DICT ? o : NULL;
    }
    return info_ref->type == TSPDF_OBJ_DICT ? info_ref : NULL;
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
            tspr_write_hex_string(buf, enc, enc_len);
            free(enc);
            return;
        }
    }
    tspr_write_string_escaped(buf, data, len);
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

// --- Merged Info emitter (TSPDF_INFO_WRITE_MERGED) ---

// Build and write an Info dict object at current buffer position.
// Merges original Info dict fields with overrides from doc->metadata.
// When `crypt` is non-NULL the object is being written into an encrypted
// file: every string value is encrypted with the Info object's own key.
void tspdf_info_emit_merged(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                            uint32_t info_obj_num, TspdfCrypt *crypt) {
    // Resolve original Info dict if present. Resolved with the document's own
    // crypt so preserved values from an encrypted source arrive decrypted
    // (they are re-encrypted with the new object's key on write below).
    TspdfObj *orig_info = source_info_dict(doc, parser);

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
                    tspr_write_name(buf, (const uint8_t *)key, strlen(key));
                    tspdf_buffer_append_byte(buf, ' ');
                    write_info_utf8_value(buf, crypt, info_obj_num, parser->arena, override_val);
                    tspdf_buffer_append_byte(buf, ' ');
                }
            } else {
                // Keep original value (skip refs, copy simple values)
                if (val && val->type == TSPDF_OBJ_STRING) {
                    tspr_write_name(buf, (const uint8_t *)key, strlen(key));
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

            tspr_write_name(buf, (const uint8_t *)fname, strlen(fname));
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
        tspr_write_name(buf, (const uint8_t *)"CreationDate", 12);
        tspdf_buffer_append_byte(buf, ' ');
        write_info_string_bytes(buf, crypt, info_obj_num,
                                (const uint8_t *)mod_date, strlen(mod_date));
        tspdf_buffer_append_byte(buf, ' ');
    }

    // Always add /ModDate
    tspr_write_name(buf, (const uint8_t *)"ModDate", 7);
    tspdf_buffer_append_byte(buf, ' ');
    write_info_string_bytes(buf, crypt, info_obj_num,
                            (const uint8_t *)mod_date, strlen(mod_date));
    tspdf_buffer_append_str(buf, " >>\nendobj\n");
}

// --- Producer-only Info emitter (TSPDF_INFO_WRITE_PRODUCER) ---

// A Producer refresh must not cost the document the rest of its Info: copy the
// original fields (minus /Producer) and stamp the new Producer. Two source
// paths use this, and they render source fields differently:
//   - identity_map != NULL (preserve-object-ids path): every source value is
//     written verbatim via tspr_write_obj under an identity renumber map.
//   - identity_map == NULL (standard path): only string-typed fields are
//     copied, as plain literal strings.
// This emitter is unencrypted-only (encrypted saves carry the source Info).
void tspdf_info_emit_producer(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                              uint32_t info_obj_num, const RenumberMap *identity_map) {
    tspdf_buffer_printf(buf, "%u 0 obj\n<< ", info_obj_num);

    TspdfObj *orig_info = source_info_dict(doc, parser);
    if (orig_info && orig_info->type == TSPDF_OBJ_DICT) {
        for (size_t j = 0; j < orig_info->dict.count; j++) {
            const char *key = orig_info->dict.entries[j].key;
            TspdfObj *val = orig_info->dict.entries[j].value;
            if (strcmp(key, "Producer") == 0) continue;
            if (identity_map) {
                tspr_write_name(buf, (const uint8_t *)key, strlen(key));
                tspdf_buffer_append_byte(buf, ' ');
                tspr_write_obj(buf, val, identity_map, doc);
                tspdf_buffer_append_byte(buf, ' ');
            } else {
                if (!val || val->type != TSPDF_OBJ_STRING) continue;
                tspr_write_name(buf, (const uint8_t *)key, strlen(key));
                tspdf_buffer_append_byte(buf, ' ');
                tspr_write_string_escaped(buf, val->string.data, val->string.len);
                tspdf_buffer_append_byte(buf, ' ');
            }
        }
    }
    tspdf_buffer_append_str(buf, "/Producer (tspdf " TSPDF_VERSION_STRING ") >>\nendobj\n");
}

// --- The decision ---

TspdfInfoPlan tspdf_info_plan(TspdfReader *doc, TspdfParser *parser,
                              const TspdfSaveOptions *opts, bool encrypted) {
    TspdfInfoPlan plan = { TSPDF_INFO_NONE, 0 };

    // Source Info object number from the trailer (indirect reference only).
    // A direct (inline) trailer Info dict has no object number; it can still
    // be merged, but cannot be carried, so it never sets source_info_num.
    uint32_t source_info_num = 0;
    bool source_is_direct_dict = false;
    if (doc->xref.trailer) {
        TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
        if (info_ref && info_ref->type == TSPDF_OBJ_REF &&
            info_ref->ref.num < doc->xref.count) {
            source_info_num = info_ref->ref.num;
        } else if (info_ref && info_ref->type == TSPDF_OBJ_DICT) {
            source_is_direct_dict = true;
        }
    }
    plan.source_info_num = source_info_num;

    // strip_metadata drops /Info entirely. (The encrypted writer never strips —
    // callers pass opts=NULL there, guarded below.)
    if (opts && opts->strip_metadata) {
        plan.action = TSPDF_INFO_NONE;
        return plan;
    }

    // Metadata edits always win: emit a freshly merged Info object.
    if (tspdf_metadata_has_changes(doc->metadata)) {
        plan.action = TSPDF_INFO_WRITE_MERGED;
        return plan;
    }

    // No edits. Encrypted saves carry the source Info (re-encrypting its
    // strings with its own key) rather than stamping Producer — Producer
    // stamping is a plain-path-only refresh.
    if (!encrypted && opts && opts->update_producer) {
        plan.action = TSPDF_INFO_WRITE_PRODUCER;
        return plan;
    }

    // A direct trailer Info dict cannot be carried by object number; rebuild
    // it as a fresh (merge-shaped) indirect object instead.
    if (source_is_direct_dict) {
        plan.action = TSPDF_INFO_WRITE_MERGED;
        return plan;
    }

    // Carry the source Info object if it resolves to a real dict; otherwise
    // there is nothing to write.
    if (source_info_num > 0) {
        TspdfObj *cand = source_info_dict(doc, parser);
        if (cand && cand->type == TSPDF_OBJ_DICT) {
            plan.action = TSPDF_INFO_CARRY_SOURCE;
            return plan;
        }
    }

    plan.action = TSPDF_INFO_NONE;
    return plan;
}
