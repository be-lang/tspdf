#ifndef TSPDF_READER_H
#define TSPDF_READER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../tspdf_error.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- PDF object types ---

typedef enum {
    TSPDF_OBJ_NULL,
    TSPDF_OBJ_BOOL,
    TSPDF_OBJ_INT,
    TSPDF_OBJ_REAL,
    TSPDF_OBJ_STRING,
    TSPDF_OBJ_NAME,
    TSPDF_OBJ_ARRAY,
    TSPDF_OBJ_DICT,
    TSPDF_OBJ_STREAM,
    TSPDF_OBJ_REF,
} TspdfObjType;

typedef struct TspdfDictEntry TspdfDictEntry;
typedef struct TspdfObj TspdfObj;

struct TspdfDictEntry {
    char *key;      // name without leading '/', arena-allocated
    TspdfObj *value;  // arena-allocated
};

struct TspdfObj {
    TspdfObjType type;
    union {
        bool boolean;
        int64_t integer;
        double real;
        struct { uint8_t *data; size_t len; } string;   // also used for TSPDF_OBJ_NAME
        struct { TspdfObj *items; size_t count; } array;
        struct { TspdfDictEntry *entries; size_t count; } dict;
        struct {
            TspdfObj *dict;          // always TSPDF_OBJ_DICT
            uint8_t *data;          // decompressed data (NULL until accessed, malloc'd)
            size_t len;             // decompressed length
            size_t raw_offset;      // offset in source buffer
            size_t raw_len;         // raw length in source buffer
            bool self_contained;    // if true, write stream.data directly (len bytes)
        } stream;
        struct { uint32_t num; uint16_t gen; } ref;
    };
};

// --- Page ---

typedef struct {
    uint32_t obj_num;
    TspdfObj *page_dict;
    double media_box[4];    // [x0, y0, x1, y1]
    double user_unit;       // page user-space scale factor, default 1.0
    int rotate;             // 0, 90, 180, 270
} TspdfReaderPage;

// --- Document (opaque) ---

typedef struct TspdfReader TspdfReader;

// --- API ---

// Open/close
TspdfReader *tspdf_reader_open(const uint8_t *data, size_t len, TspdfError *err);
TspdfReader *tspdf_reader_open_file(const char *path, TspdfError *err);
void tspdf_reader_destroy(TspdfReader *doc);

// Query
size_t tspdf_reader_page_count(const TspdfReader *doc);
TspdfReaderPage *tspdf_reader_get_page(const TspdfReader *doc, size_t index);

// PDF version, e.g. "1.7": the catalog /Version name when present,
// otherwise the file header. Owned by the reader; valid until destroy.
const char *tspdf_reader_pdf_version(const TspdfReader *doc);

// Encryption details of an encrypted document opened with a password.
// Returns false (outputs untouched) if the document is not encrypted.
// *algorithm is a static string such as "RC4-40", "RC4-128", "AES-128"
// or "AES-256"; *revision is the /R value (2-6).
bool tspdf_reader_encryption_info(const TspdfReader *doc, int *revision,
                                  const char **algorithm);

// Catalog presence flags: document outline (bookmarks) and AcroForm.
bool tspdf_reader_has_outlines(const TspdfReader *doc);
bool tspdf_reader_has_acroform(const TspdfReader *doc);

// Manipulate (returns new document, source unchanged).
// The source document must outlive the returned document unless saved first,
// because the returned document references the source's stream data.
// Merge copies stream data, so merged documents are self-contained.
TspdfReader *tspdf_reader_extract(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err);
TspdfReader *tspdf_reader_delete(TspdfReader *doc, const size_t *pages, size_t count, TspdfError *err);
TspdfReader *tspdf_reader_rotate(TspdfReader *doc, const size_t *pages, size_t count, int angle, TspdfError *err);
TspdfReader *tspdf_reader_reorder(TspdfReader *doc, const size_t *order, size_t count, TspdfError *err);

// Set the /CropBox of the given pages to `box` = [x0 y0 x1 y1] in the page's
// default user space (the same coordinate space as the /MediaBox; not
// relative to the MediaBox origin). This is non-destructive: page content is
// preserved and merely clipped to the new visible region. The box is
// intersected with each page's MediaBox (a CropBox outside the MediaBox is
// meaningless per the spec), so the stored box never exceeds the media. A
// degenerate box (x1<=x0 or y1<=y0, before or after clamping) yields
// TSPDF_ERR_INVALID_ARG. Out-of-range page indices yield TSPDF_ERR_PAGE_RANGE.
// Returns a new document (source unchanged), consistent with rotate/extract.
TspdfReader *tspdf_reader_set_cropbox(TspdfReader *doc, const size_t *pages,
                                      size_t count, const double box[4],
                                      TspdfError *err);

// Like tspdf_reader_set_cropbox but with a distinct box per listed page:
// boxes[i] (four doubles) is the CropBox for pages[i]. Lets a caller crop
// pages of differing MediaBox sizes (e.g. margin-based cropping) in one call.
TspdfReader *tspdf_reader_set_cropboxes(TspdfReader *doc, const size_t *pages,
                                        size_t count, const double *boxes,
                                        TspdfError *err);

// Scale the given pages by (sx, sy): the page content is wrapped in a
// `q sx 0 0 sy 0 0 cm ... Q` transform and the /MediaBox (and any /CropBox)
// is multiplied by the same factors, so the page grows/shrinks with its
// content rather than being clipped. Existing box origins are scaled too.
// A rotated page (/Rotate) is handled: the transform is applied in unrotated
// page space, which the viewer then rotates, so content still fills the page.
// Non-positive factors yield TSPDF_ERR_INVALID_ARG; out-of-range indices yield
// TSPDF_ERR_PAGE_RANGE. Returns a new document (source unchanged).
TspdfReader *tspdf_reader_scale(TspdfReader *doc, const size_t *pages,
                                size_t count, double sx, double sy,
                                TspdfError *err);

// Resize the given pages to a target media size (target_w x target_h, in
// points). Each page's content is uniformly scaled to fit the target while
// preserving aspect ratio and centered, and the /MediaBox becomes
// [0 0 target_w target_h]; any /CropBox is dropped (it no longer applies).
// For a page whose /Rotate is 90 or 270 the target dimensions are matched in
// the viewed orientation, so the page still fits upright. Non-positive target
// dimensions yield TSPDF_ERR_INVALID_ARG. Returns a new document.
TspdfReader *tspdf_reader_resize_to(TspdfReader *doc, const size_t *pages,
                                    size_t count, double target_w, double target_h,
                                    TspdfError *err);
TspdfReader *tspdf_reader_merge(TspdfReader **docs, size_t count, TspdfError *err);

// Save
TspdfError tspdf_reader_save(TspdfReader *doc, const char *path);
TspdfError tspdf_reader_save_to_memory(TspdfReader *doc, uint8_t **out, size_t *out_len);

// Open encrypted PDF with password
TspdfReader *tspdf_reader_open_with_password(const uint8_t *data, size_t len,
                                                const char *password, TspdfError *err);
TspdfReader *tspdf_reader_open_file_with_password(const char *path,
                                                     const char *password, TspdfError *err);

// Save with encryption (key_bits: 128=AES-128/V4R4, 256=AES-256/V5R6)
TspdfError tspdf_reader_save_encrypted(TspdfReader *doc, const char *path,
                                        const char *user_pass, const char *owner_pass,
                                        uint32_t permissions, int key_bits);
TspdfError tspdf_reader_save_to_memory_encrypted(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                  const char *user_pass, const char *owner_pass,
                                                  uint32_t permissions, int key_bits);

// Metadata getters (return NULL if not present)
const char *tspdf_reader_get_title(const TspdfReader *doc);
const char *tspdf_reader_get_author(const TspdfReader *doc);
const char *tspdf_reader_get_subject(const TspdfReader *doc);
const char *tspdf_reader_get_keywords(const TspdfReader *doc);
const char *tspdf_reader_get_creator(const TspdfReader *doc);
const char *tspdf_reader_get_producer(const TspdfReader *doc);
const char *tspdf_reader_get_creation_date(const TspdfReader *doc);
const char *tspdf_reader_get_mod_date(const TspdfReader *doc);

// Metadata setters (pass NULL to remove a field)
void tspdf_reader_set_title(TspdfReader *doc, const char *value);
void tspdf_reader_set_author(TspdfReader *doc, const char *value);
void tspdf_reader_set_subject(TspdfReader *doc, const char *value);
void tspdf_reader_set_keywords(TspdfReader *doc, const char *value);
void tspdf_reader_set_creator(TspdfReader *doc, const char *value);
void tspdf_reader_set_producer(TspdfReader *doc, const char *value);

// --- Save options ---

typedef struct {
    bool preserve_object_ids;   // Keep original object numbers (enables raw byte copy, faster)
    bool strip_unused_objects;  // Only include objects reachable from page tree
    bool use_xref_stream;      // Use compact xref stream (PDF 1.5+) instead of classic table
    bool recompress_streams;   // Minimize stream storage: deflate unfiltered streams,
                               // re-deflate FlateDecode ones (keeping whichever encoding
                               // is smaller), and write the xref as a compressed stream
    bool strip_metadata;       // Remove Info dict and XMP metadata
    bool update_producer;      // Set /Producer to "tspdf" (default true)
} TspdfSaveOptions;

TspdfSaveOptions tspdf_save_options_default(void);
TspdfError tspdf_reader_save_with_options(TspdfReader *doc, const char *path,
                                          const TspdfSaveOptions *opts);
TspdfError tspdf_reader_save_to_memory_with_options(TspdfReader *doc, uint8_t **out, size_t *out_len,
                                                     const TspdfSaveOptions *opts);

// --- Text extraction ---

// Extract the text of one page as UTF-8, in content-stream order (no column
// re-ordering). Line breaks are derived from the text matrix; glyphs are
// decoded via /ToUnicode, then /Encoding (+/Differences), then
// StandardEncoding; unmappable glyphs become U+FFFD. The returned string is
// owned by the reader's arena (valid until tspdf_reader_destroy); do not
// free. Returns NULL and sets *err on failure (TSPDF_ERR_PAGE_RANGE for a
// bad page index).
const char *tspdf_reader_page_text(TspdfReader *doc, size_t page_index, TspdfError *err);

// --- AcroForm form fields ---

typedef enum {
    TSPDF_FIELD_TEXT,        // /FT /Tx
    TSPDF_FIELD_CHECKBOX,    // /FT /Btn without the radio/pushbutton flags
    TSPDF_FIELD_RADIO,       // /FT /Btn with /Ff bit 16
    TSPDF_FIELD_PUSHBUTTON,  // /FT /Btn with /Ff bit 17
    TSPDF_FIELD_CHOICE,      // /FT /Ch
    TSPDF_FIELD_SIGNATURE,   // /FT /Sig
    TSPDF_FIELD_UNKNOWN,     // missing or unrecognized /FT
} TspdfFieldType;

typedef struct {
    const char *name;        // fully-qualified dotted name (UTF-8); "" if unnamed
    TspdfFieldType type;
    const char *value;       // current /V as UTF-8, or NULL when unset
    bool readonly;           // /Ff bit 1
    bool required;           // /Ff bit 2
    size_t page_index;       // 0-based page of the first widget; SIZE_MAX unknown
    double rect[4];          // first widget /Rect [x0 y0 x1 y1]; zeros if absent
    const char **options;    // checkbox/radio: "on" state names (/AP /N keys
                             // != /Off); choice: /Opt export values
    size_t option_count;
} TspdfFormFieldInfo;

// Enumerate the terminal fields of the document's AcroForm. *out_fields and
// everything it points to are owned by the reader (valid until destroy; do
// not free). A document without a form yields TSPDF_OK and count 0.
TspdfError tspdf_reader_form_fields(TspdfReader *doc,
                                    TspdfFormFieldInfo **out_fields,
                                    size_t *out_count);

// Set the value of the named field (fully-qualified name as returned by
// tspdf_reader_form_fields). Text/choice fields take any UTF-8 string;
// checkbox/radio fields take an "on" state name or "Off". Text fields get a
// regenerated appearance stream (single-line; no comb/multiline support) and
// the AcroForm is marked NeedAppearances. Unknown names return
// TSPDF_ERR_INVALID_ARG; readonly fields return TSPDF_ERR_UNSUPPORTED unless
// `force` is set.
TspdfError tspdf_reader_form_fill(TspdfReader *doc, const char *name,
                                  const char *value, bool force);

// Stamp every field's current value into the page content (text values and
// button check marks), then remove all widget annotations and the catalog
// /AcroForm so the result is a plain, non-interactive document.
TspdfError tspdf_reader_form_flatten(TspdfReader *doc);
// Layout-preserving variant (pdftotext -layout style): fragments are placed
// on a character grid by their device-space positions, so columns and tables
// stay visually aligned. Lines run top to bottom, fragments left to right;
// vertical gaps beyond ~1.8 line heights become blank lines (at most 2).
// Decoding, ownership and errors are identical to tspdf_reader_page_text.
const char *tspdf_reader_page_text_layout(TspdfReader *doc, size_t page_index,
                                          TspdfError *err);
// --- Embedded file attachments ---

typedef struct {
    const char *name;   // name-tree key (stored filename), NUL-terminated
    size_t size;        // decoded (uncompressed) size in bytes
    const char *desc;   // Filespec /Desc if present, else NULL
    const char *mime;   // embedded stream /Subtype (MIME type) if present, else NULL
} TspdfAttachmentInfo;

// List document-level embedded files (/Names /EmbeddedFiles). On success
// *out points at an array of *count entries owned by the reader's arena
// (valid until tspdf_reader_destroy; do not free). *out is NULL when the
// document has no attachments.
TspdfError tspdf_reader_attachments(TspdfReader *doc, TspdfAttachmentInfo **out,
                                    size_t *count);

// Fetch the decoded bytes of the attachment stored under `name`. *out is
// malloc'd (caller frees). TSPDF_ERR_NOT_FOUND when no attachment has that
// name.
TspdfError tspdf_reader_attachment_get(TspdfReader *doc, const char *name,
                                       uint8_t **out, size_t *out_len);

// Add (or replace, by name) an embedded file. Creates the catalog /Names
// dictionary and /EmbeddedFiles tree when absent; the tree is always written
// as a single flat node with lexicographically sorted keys. `desc` may be
// NULL. The bytes are copied; the document must be saved for the attachment
// to persist.
TspdfError tspdf_reader_attachment_add(TspdfReader *doc, const char *name,
                                       const uint8_t *data, size_t len,
                                       const char *desc);

// Remove the attachment stored under `name`. TSPDF_ERR_NOT_FOUND when no
// attachment has that name.
TspdfError tspdf_reader_attachment_remove(TspdfReader *doc, const char *name);
// --- Page import (stamping) ---

// Wrap a page of `src` as a /Form XObject in `dst`: the source page's content
// stream(s) become the XObject stream, its /Resources are deep-copied (the
// whole referenced object graph is imported), and BBox is the source page
// MediaBox. The result is self-contained: `src` may be destroyed immediately
// after this call, before `dst` is saved. Encrypted sources are decrypted
// during the copy. BBox coordinates are clamped to +/-1e7.
//
// Returns the new object number in `dst` (use tspdf_page_add_xobject to
// reference it from a page), or 0 on failure with *err set. out_bbox
// (optional) receives the XObject BBox [x0 y0 x1 y1].
uint32_t tspdf_reader_import_page_xobject(TspdfReader *dst, TspdfReader *src,
                                          size_t src_page_index, double out_bbox[4],
                                          TspdfError *err);

// Register an XObject (e.g. from tspdf_reader_import_page_xobject) in a
// page's /Resources under a fresh name. Returns the name to use with the Do
// operator (owned by the reader, valid until destroy), or NULL on failure.
const char *tspdf_page_add_xobject(TspdfReader *doc, size_t page_index,
                                   uint32_t xobj_num);

// --- Annotations ---

TspdfError tspdf_page_add_link(TspdfReader *doc, size_t page_index,
                              double x, double y, double w, double h,
                              const char *url);
TspdfError tspdf_page_add_link_to_page(TspdfReader *doc, size_t page_index,
                                      double x, double y, double w, double h,
                                      size_t target_page);
TspdfError tspdf_page_add_text_note(TspdfReader *doc, size_t page_index,
                                   double x, double y,
                                   const char *title, const char *contents);
TspdfError tspdf_page_add_stamp(TspdfReader *doc, size_t page_index,
                               double x, double y, double w, double h,
                               const char *stamp_name);

#ifdef __cplusplus
}
#endif

#endif
