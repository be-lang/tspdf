#define _POSIX_C_SOURCE 200809L
#include "tspr_internal.h"
#include "tspr_doctree.h"
#include "../util/pdftext.h"
#include <stdlib.h>
#include <string.h>

static TspdfObj *get_info_dict(const TspdfReader *doc) {
    if (!doc || !doc->xref.trailer) return NULL;
    TspdfObj *info_ref = tspdf_dict_get(doc->xref.trailer, "Info");
    if (!info_ref) return NULL;
    if (info_ref->type == TSPDF_OBJ_REF) {
        uint32_t num = info_ref->ref.num;
        if (num >= doc->xref.count) return NULL;
        if (doc->obj_cache[num]) return doc->obj_cache[num];
        // Not cached — resolve it
        TspdfReader *mut_doc = (TspdfReader *)doc;
        TspdfParser p;
        tspdf_parser_init(&p, mut_doc->data, mut_doc->data_len, &mut_doc->arena);
        return tspdf_xref_resolve(&mut_doc->xref, &p, num, mut_doc->obj_cache, mut_doc->crypt);
    }
    if (info_ref->type == TSPDF_OBJ_DICT) return info_ref;
    return NULL;
}

static const char *get_info_field(const TspdfReader *doc, const char *key) {
    TspdfObj *info = get_info_dict(doc);
    if (!info || info->type != TSPDF_OBJ_DICT) return NULL;
    TspdfObj *val = tspdf_dict_get(info, key);
    if (!val || val->type != TSPDF_OBJ_STRING) return NULL;
    // Info values are PDF text strings (ISO 32000 §7.9.2.2): UTF-16BE with
    // BOM, or PDFDocEncoding otherwise. Decode to UTF-8 for display; the
    // arena keeps the copy alive for the lifetime of the document.
    TspdfReader *mut_doc = (TspdfReader *)doc;
    return tspdf_pdf_text_to_utf8(val->string.data, val->string.len,
                                  &mut_doc->arena);
}

const char *tspdf_reader_get_title(const TspdfReader *doc) { return get_info_field(doc, "Title"); }
const char *tspdf_reader_get_author(const TspdfReader *doc) { return get_info_field(doc, "Author"); }
const char *tspdf_reader_get_subject(const TspdfReader *doc) { return get_info_field(doc, "Subject"); }
const char *tspdf_reader_get_keywords(const TspdfReader *doc) { return get_info_field(doc, "Keywords"); }
const char *tspdf_reader_get_creator(const TspdfReader *doc) { return get_info_field(doc, "Creator"); }
const char *tspdf_reader_get_producer(const TspdfReader *doc) { return get_info_field(doc, "Producer"); }
const char *tspdf_reader_get_creation_date(const TspdfReader *doc) { return get_info_field(doc, "CreationDate"); }
const char *tspdf_reader_get_mod_date(const TspdfReader *doc) { return get_info_field(doc, "ModDate"); }

// True when the catalog carries an XMP metadata stream. The Info setters
// below never touch it, so callers use this to warn about stale XMP values.
bool tspdf_reader_has_xmp_metadata(TspdfReader *doc) {
    if (!doc) return false;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *catalog = doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT
        ? doc->catalog
        : (doc->xref.trailer
               ? tspdf_doctree_resolve(doc, &parser,
                                       tspdf_dict_get(doc->xref.trailer, "Root"))
               : NULL);
    if (!catalog || catalog->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *meta = tspdf_doctree_resolve(doc, &parser,
                                           tspdf_dict_get(catalog, "Metadata"));
    return meta && meta->type == TSPDF_OBJ_STREAM;
}

static void ensure_metadata(TspdfReader *doc) {
    if (!doc->metadata) {
        doc->metadata = calloc(1, sizeof(TspdfReaderMetadata));
    }
}

void tspdf_reader_set_title(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->title);
    doc->metadata->title = value ? strdup(value) : NULL;
    doc->metadata->title_set = true;
    doc->modified = true;
}
void tspdf_reader_set_author(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->author);
    doc->metadata->author = value ? strdup(value) : NULL;
    doc->metadata->author_set = true;
    doc->modified = true;
}
void tspdf_reader_set_subject(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->subject);
    doc->metadata->subject = value ? strdup(value) : NULL;
    doc->metadata->subject_set = true;
    doc->modified = true;
}
void tspdf_reader_set_keywords(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->keywords);
    doc->metadata->keywords = value ? strdup(value) : NULL;
    doc->metadata->keywords_set = true;
    doc->modified = true;
}
void tspdf_reader_set_creator(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->creator);
    doc->metadata->creator = value ? strdup(value) : NULL;
    doc->metadata->creator_set = true;
    doc->modified = true;
}
void tspdf_reader_set_producer(TspdfReader *doc, const char *value) {
    ensure_metadata(doc);
    free(doc->metadata->producer);
    doc->metadata->producer = value ? strdup(value) : NULL;
    doc->metadata->producer_set = true;
    doc->modified = true;
}

