#ifndef TSPDF_READER_INTERNAL_H
#define TSPDF_READER_INTERNAL_H

#include "tspr.h"
#include "../util/arena.h"
#include "../crypto/aes.h"

// --- Parser ---

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    TspdfArena *arena;
    TspdfError error;
    int depth;              // current array/dict nesting depth (recursion guard)
} TspdfParser;

// Maximum array/dict nesting depth. A hostile PDF with thousands of nested
// [[[...]]] or <<...>> would otherwise recurse until the C stack overflows
// (verified DoS). 256 comfortably exceeds real-world nesting.
#define TSPDF_MAX_PARSE_DEPTH 256

void tspdf_parser_init(TspdfParser *p, const uint8_t *data, size_t len, TspdfArena *arena);
void tspdf_skip_whitespace(TspdfParser *p);
TspdfObj *tspdf_parse_obj(TspdfParser *p);
TspdfObj *tspdf_parse_indirect_obj(TspdfParser *p, uint32_t *out_num, uint16_t *out_gen);

// --- Encryption ---

typedef struct {
    uint8_t file_key[32];
    int key_len;            // 5, 16, or 32 bytes
    int version;            // /V value (1-5)
    int revision;           // /R value (2-6)
    bool use_aes;           // true for AES, false for RC4
    bool identity_stm;      // V=4 StmF == Identity: streams are not encrypted
    bool identity_str;      // V=4 StrF == Identity: strings are not encrypted
    bool encrypt_metadata;  // /EncryptMetadata (default true); Algorithm 2 step (f)
    uint32_t permissions;   // /P value
    uint8_t *file_id;      // first file ID string (malloc'd)
    size_t file_id_len;
    // Encryption output fields (used by encrypt_init for serialization)
    uint8_t O[48];
    uint8_t U[48];
    uint8_t OE[32];
    uint8_t UE[32];
    uint8_t Perms[16];
    size_t O_len;           // 32 for V<=4, 48 for V=5
    size_t U_len;           // 32 for V<=4, 48 for V=5
    // V=5 only: the file key IS the object key (no per-object derivation), so
    // one AES-256 schedule serves every string/stream. Built lazily on first
    // use by tspr_crypt.c; V<=4 derives a per-object key and cannot cache.
    Aes aes_v5;
    bool aes_v5_ready;
    // Source-document /Encrypt preservation (set for crypts created by
    // tspdf_crypt_init when opening an encrypted file, NULL for crypts made
    // by tspdf_crypt_encrypt_init). The dict lives in the owning document's
    // arena; encrypted saves copy it verbatim so the original passwords (and
    // permissions) keep working — the recovered file key is reused, since
    // the "other" password can never be re-derived. src_encrypt_num is the
    // object number of the source /Encrypt dict (0 when it was inline) so
    // the serializer can skip that now-superseded object.
    TspdfObj *src_encrypt_dict;
    uint32_t src_encrypt_num;
} TspdfCrypt;

// --- Metadata ---

typedef struct {
    char *title, *author, *subject, *keywords, *creator, *producer;
    bool title_set, author_set, subject_set, keywords_set, creator_set, producer_set;
    // "D:YYYYMMDDHHmmSS" stamped into xmp:ModifyDate by the XMP sync
    // (tspr_xmp.c); the next save reuses it for Info /ModDate so the two
    // stay equal. NULL when no sync edited the packet.
    char *mod_date;
} TspdfReaderMetadata;

// True when any metadata override field is set (title/author/subject/keywords/
// creator/producer). Defined in tspr_infoplan.c.
bool tspdf_metadata_has_changes(const TspdfReaderMetadata *m);

// --- Xref ---

typedef struct {
    size_t offset;
    uint16_t gen;
    bool in_use;
    bool compressed;
    bool seen;           // true once any xref section has set this entry
    uint32_t stream_obj;
    size_t stream_idx;
} TspdfReaderXrefEntry;

typedef struct {
    TspdfReaderXrefEntry *entries;
    size_t count;
    TspdfObj *trailer;
} TspdfReaderXref;

TspdfError tspdf_xref_parse(TspdfParser *p, TspdfReaderXref *xref);
// Last-resort rebuild: scan the whole buffer for "N G obj" markers and build a
// synthetic xref + trailer. Used by the document layer when a parsed xref does
// not yield a usable catalog (e.g. a file behind a large transfer prefix whose
// offsets are all shifted). Clears any prior entries/trailer before scanning.
TspdfError tspdf_xref_reconstruct_by_scan(TspdfParser *p, TspdfReaderXref *xref);
TspdfObj *tspdf_xref_resolve(TspdfReaderXref *xref, TspdfParser *p, uint32_t obj_num, TspdfObj **cache, TspdfCrypt *crypt);

// --- Pages ---

typedef struct {
    TspdfReaderPage *pages;
    size_t count;
} TspdfReaderPageList;

TspdfError tspdf_pages_load(TspdfParser *p, TspdfReaderXref *xref, TspdfObj **cache,
                          TspdfObj *catalog, TspdfReaderPageList *out, TspdfCrypt *crypt);

// --- Serialization ---

#include "../util/buffer.h"

TspdfError tspdf_serialize(TspdfReader *doc, uint8_t **out_buf, size_t *out_len);
TspdfError tspdf_serialize_with_options(TspdfReader *doc, uint8_t **out_buf, size_t *out_len,
                                      const TspdfSaveOptions *opts);

// Renumbering map: old_to_new[old_num] = new_num, 0 means not mapped. An
// identity map (old_to_new[i] == i) is used by the preserve-object-ids path.
typedef struct {
    uint32_t *old_to_new;
    size_t map_size;
} RenumberMap;

// Low-level PDF value writers (defined in tspr_serialize.c). Exposed so the
// Info-plan module can emit Info-dict objects without duplicating them.
void tspr_write_string_escaped(TspdfBuffer *buf, const uint8_t *data, size_t len);
void tspr_write_hex_string(TspdfBuffer *buf, const uint8_t *data, size_t len);
void tspr_write_name(TspdfBuffer *buf, const uint8_t *data, size_t len);
void tspr_write_obj(TspdfBuffer *buf, TspdfObj *obj, const RenumberMap *map, TspdfReader *doc);

// --- Info dict plan (tspr_infoplan.c) ---
//
// One module decides what happens to the trailer /Info dictionary on save and
// emits the resulting object. Historically this was spread across the plain
// and encrypted save paths, which drifted apart and shipped three bugs (Info
// dropped on encrypt; a producer refresh wiping Title/Author; Info strings
// written plaintext into encrypted files). Both save paths are now callers.

typedef enum {
    TSPDF_INFO_NONE,           // no Info object (strip_metadata, or nothing to write)
    TSPDF_INFO_CARRY_SOURCE,   // reuse the source Info object at its own number
    TSPDF_INFO_WRITE_MERGED,   // emit a fresh merged Info (metadata edits)
    TSPDF_INFO_WRITE_PRODUCER  // emit a fresh Info that only stamps /Producer
} TspdfInfoAction;

typedef struct {
    TspdfInfoAction action;
    uint32_t source_info_num;  // source Info object number (valid unless NONE
                               // with no source); 0 when the trailer has none
} TspdfInfoPlan;

// Decide what happens to /Info given the metadata edits, update_producer,
// strip_metadata, and whether this save writes an encrypted file. Encrypted
// saves never stamp Producer alone — they carry the source Info instead (its
// strings are re-encrypted with its own key), so `encrypted` selects CARRY
// over WRITE_PRODUCER. `parser` resolves the source Info dict.
TspdfInfoPlan tspdf_info_plan(TspdfReader *doc, TspdfParser *parser,
                              const TspdfSaveOptions *opts, bool encrypted);

// Emit a complete merged Info object (`N 0 obj ... endobj`) at the current
// buffer position: source fields, metadata overrides, /CreationDate, /ModDate.
// When `crypt` is non-NULL every string value is encrypted with the Info
// object's own key (ISO 32000 §7.6.2). Corresponds to TSPDF_INFO_WRITE_MERGED.
void tspdf_info_emit_merged(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                            uint32_t info_obj_num, TspdfCrypt *crypt);

// Emit a fresh Info object that copies the source fields minus /Producer and
// stamps the current tspdf Producer. Corresponds to TSPDF_INFO_WRITE_PRODUCER.
// With `identity_map` non-NULL (the preserve-object-ids path) every source
// value is written verbatim via tspr_write_obj; with NULL (the standard path)
// only string-typed source fields are copied, as plain literal strings.
// This emitter is only used for unencrypted saves.
void tspdf_info_emit_producer(TspdfBuffer *buf, TspdfReader *doc, TspdfParser *parser,
                              uint32_t info_obj_num, const RenumberMap *identity_map);

// Shared stream classifiers (defined in tspr_serialize.c) used by both the
// writer and the stream/collection planners in tspr_streamplan.c.
//   - tspr_stream_dict_type_is: stream with /Type equal to the given name.
//   - tspr_is_xref_machinery: an ObjStm or XRef container (dropped on rewrite).
//   - tspr_is_xmp_metadata: a /Type /Metadata stream or dict (XMP packet).
bool tspr_stream_dict_type_is(TspdfObj *obj, const char *type_name);
bool tspr_is_xref_machinery(TspdfObj *obj);
bool tspr_is_xmp_metadata(TspdfObj *obj);

// --- Stream recompression plan (tspr_streamplan.c) ---
//
// One decision function classifies what happens to a stream on a recompressing
// save, so write_stream_obj_with_options is a thin executor. The pure decision
// (which strategy applies) is separated from the byte work (running deflate and
// the never-grow size check) so it can be matrix-tested without a full save.
//
// History: the classification used to live inline as six intertwined branches
// in the writer; the indirect-/DecodeParms armor-strip refusal (a shipped-bug
// fix) is the fragile part and is pinned by the test matrix.

typedef enum {
    // Write the stream verbatim (no recompression eligible, or recompress off).
    TSPDF_STREAM_KEEP,
    // Inflate + re-deflate a bare /FlateDecode stream; keep the smaller of the
    // two encodings. /Filter and any predictor /DecodeParms are preserved.
    TSPDF_STREAM_REDEFLATE,
    // Deflate an unfiltered stream and add /Filter /FlateDecode when smaller.
    TSPDF_STREAM_ADD_FLATE,
    // Decode a strippable armor chain (ASCIIHex/85/RunLength/LZW, possibly over
    // Flate) fully and re-encode as plain Flate; drop the old /Filter and
    // /DecodeParms (/DP). Refused when /DecodeParms is not fully direct.
    TSPDF_STREAM_ARMOR_STRIP
} TspdfStreamAction;

typedef struct {
    TspdfStreamAction action;
    // Image XObjects use the fast deflate level (their predictor-coded Flate
    // gains almost nothing from the best-effort search). Meaningful for the
    // REDEFLATE, ADD_FLATE, and ARMOR_STRIP actions.
    bool use_fast_level;
} TspdfStreamPlan;

// Classify a stream for a recompressing save from its dict alone (no bytes
// read, no deflate run). `stream_len` is the source stream length; it only
// gates the ADD_FLATE empty-stream case (len 0 is never compressed).
// `recompress` is opts->recompress_streams; `is_xref` skips XRef streams.
// The executor still applies the never-grow size check after running deflate.
TspdfStreamPlan tspdf_stream_plan(TspdfObj *stream_obj, bool recompress,
                                  bool is_xref, size_t stream_len);

// --- Save-object collection (tspr_streamplan.c) ---
//
// The three save paths (plain-standard, encrypted, and the collection part of
// preserve-ids) walked the same visited[] array and then filtered it into a
// collected[] list with near-identical loops. This shared walk returns the
// filtered list plus the renumber map, so the paths keep their own EMISSION
// but stop duplicating the collection/decision logic.

typedef struct {
    uint32_t *collected;     // malloc'd list of source object numbers, in order
    size_t collected_count;
    RenumberMap map;         // old_to_new for the collected objects (from base_num)
    bool ok;                 // false on allocation failure (nothing to free)
} TspdfCollectResult;

// Filter a visited[] mark array into the ordered object list every re-serialize
// path emits, and build the renumber map assigning new numbers from `base_num`
// upward in collection order. Drops xref machinery (ObjStm/XRef containers)
// always; drops `source_root_num` when `skip_root` (the synthetic-catalog
// paths); drops the source Info object and XMP metadata streams when
// `strip_metadata`. Both the plain and encrypted standard paths call this.
// On success res->ok is true and res->collected/res->map.old_to_new are
// malloc'd (caller frees). On allocation failure res->ok is false.
void tspdf_collect_objects(TspdfReader *doc, TspdfParser *parser,
                           const bool *visited, size_t total_objs,
                           uint32_t base_num, bool skip_root,
                           uint32_t source_root_num, bool strip_metadata,
                           uint32_t source_info_num,
                           TspdfCollectResult *res);

// --- New object list (for Phase 3 content overlay / annotations) ---

typedef struct {
    TspdfObj **objs;
    size_t count;
    size_t capacity;
} TspdfNewObjList;

// --- Form fallback font (tspr_form.c) ---

// Cached fallback font for form appearance streams whose value contains
// characters outside cp1252 (defined in tspr_form.c; freed by
// tspdf_form_fallback_free from tspdf_reader_destroy).
typedef struct TspdfFormFallback TspdfFormFallback;

// --- Cross-import dedup cache (tspr_import.c) ---
//
// Maps (source reader, source object number) -> destination object number so
// repeated imports from the same source into one destination reuse the copy
// made by an earlier tspdf_reader_import_page_xobject call. nup and stamp
// import once per placement/page, so without this every placement re-copied
// the shared resources (FontFile streams etc.), bloating the output.
//
// Constraints (keys hold the source by pointer):
//   1. The source must outlive the destination until the destination is saved
//      — the documented derived-document rule (see the "Manipulate" section of
//      tspr.h: "The source document must outlive the returned document unless
//      saved first"). Corollary: if a source IS destroyed early (which the
//      import API itself permits) and a new reader happens to be allocated at
//      the same address, a later import from it into the same destination
//      could alias a stale key. Acceptable under rule 1; no CLI flow does it.
//   2. Mutating a source between imports into the same destination leaves the
//      cached copies stale (the dedup returns the pre-mutation import). The
//      CLI flows (nup, stamp, watermark) never mutate the source; this is a
//      documented constraint of the import API, not detected at runtime.
// Generation numbers are not part of the key: imports always rewrite refs to
// generation 0 and registered objects serialize as gen 0, so the source gen
// is irrelevant after the copy.
typedef struct {
    const TspdfReader *src;  // source reader identity (never dereferenced)
    uint32_t src_num;        // object number in the source
    uint32_t dst_num;        // object number of the imported copy in dst
} TspdfImportCacheEntry;

typedef struct {
    TspdfImportCacheEntry *entries;  // sorted by (src, src_num) for bsearch
    size_t count;
    size_t capacity;
} TspdfImportCache;

// --- Document (full definition) ---

struct TspdfReader {
    const uint8_t *data;
    size_t data_len;
    bool owns_data;
    TspdfArena arena;
    TspdfReaderXref xref;
    TspdfReaderPageList pages;
    TspdfObj *catalog;
    TspdfObj **obj_cache;
    char pdf_version[8];    // e.g. "1.7"
    TspdfCrypt *crypt;           // non-NULL if encrypted (malloc'd)
    TspdfReaderMetadata *metadata;     // non-NULL if metadata modified (malloc'd)
    TspdfNewObjList new_objs;    // objects created by Phase 3 APIs
    TspdfImportCache import_cache;  // cross-import dedup (see typedef above)
    bool modified;              // set by manipulation/annotation/metadata functions
    TspdfFormFallback *form_fallback;  // lazy fallback font cache (malloc'd)

    // --- Derived-document lifetime (see the "Manipulate" section of tspr.h) ---
    // A derived document (extract/delete/rotate/nup, ...) aliases its source's
    // data buffer, xref and (when encrypted) crypt clone. To make the interface
    // defend itself, the derived doc holds a reference on its source:
    //   derived_source  -> the immediate source (NULL for a self-contained doc,
    //                      e.g. a freshly opened file or a merge output). Chains
    //                      form naturally: a derived-of-derived points at its
    //                      parent derived doc, which points at the original.
    //   derived_refs    -> number of live derived docs that reference THIS doc.
    //   destroy_pending -> tspdf_reader_destroy was called while derived_refs>0,
    //                      so the real free was deferred until the last release.
    // The handle is dead to the caller the instant destroy returns; only the
    // memory lifetime is deferred (see tspdf_reader_destroy).
    struct TspdfReader *derived_source;
    size_t derived_refs;
    bool destroy_pending;
};

// Free doc->form_fallback (safe when NULL). Defined in tspr_form.c.
void tspdf_form_fallback_free(struct TspdfReader *doc);

// Register that `derived` aliases `source`'s memory (data/xref/crypt), so a
// tspdf_reader_destroy on the source defers the free until every derived doc is
// released. Call once, on a fully-built derived reader that cannot fail after.
// Defined in tspr_document.c; used by the derived-document constructors.
void tspdf_reader_hold_source(struct TspdfReader *derived, struct TspdfReader *source);

// Resource name used in appearance streams and /DR for the embedded fallback
// font (CIDFontType2 / Identity-H). Defined here so tests can reference it
// without duplicating the literal.
#define TSPR_FORM_FALLBACK_RES_NAME "TspdfFb"

// Internal seam: generate the content-stream bytes for a text-field appearance
// (/Tx BMC ... EMC). Pure function — no document access. Exposed for testing.
// h: widget bounding box height. res_font: resource name for the font.
// size: Tf point size. line: cp1252-safe value text (or ignored when
// fallback_tj is non-NULL). fallback_tj: Identity-H hex string, or NULL.
// Returns malloc'd NUL-terminated bytes; NULL on alloc failure.
char *tspr_form_gen_text_ap_content(double h,
                                    const char *res_font, double size,
                                    const char *line,
                                    const char *fallback_tj);

// --- Crypt ---

TspdfError tspdf_crypt_init(TspdfCrypt *crypt, TspdfObj *encrypt_dict,
                          TspdfObj *trailer, const char *password);
TspdfError tspdf_crypt_decrypt_string(TspdfCrypt *crypt, uint32_t obj_num,
                                     uint16_t gen, uint8_t *data, size_t *len);
uint8_t *tspdf_crypt_decrypt_stream(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
TspdfError tspdf_crypt_encrypt_init(TspdfCrypt *crypt, const char *user_pass,
                                   const char *owner_pass, uint32_t permissions,
                                   int key_bits);
uint8_t *tspdf_crypt_encrypt_string(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
uint8_t *tspdf_crypt_encrypt_stream(TspdfCrypt *crypt, uint32_t obj_num,
                                    uint16_t gen, const uint8_t *data,
                                    size_t len, size_t *out_len);
// Clone a reader's crypt into a derived document so its save preserves the
// source encryption (malloc'd; freed by tspdf_reader_destroy). The clone's
// src_encrypt_dict points into the source's arena, which stays valid because
// the derived document holds a reference on the source (tspdf_reader_hold_source
// defers the source's arena free until the derived document is destroyed).
TspdfCrypt *tspdf_crypt_clone(const TspdfCrypt *src);
void tspdf_random_bytes(uint8_t *buf, size_t len);
TspdfError tspdf_serialize_encrypted(TspdfReader *doc, TspdfCrypt *crypt,
                                    uint8_t **out_buf, size_t *out_len);

// --- Resource merging ---

typedef struct {
    char *old_name;     // arena-allocated
    char *new_name;     // arena-allocated
} TspdfResourceRename;

typedef struct {
    TspdfResourceRename *renames;    // arena-allocated array
    size_t count;
    size_t capacity;
} TspdfRenameMap;

TspdfError tspdf_resources_merge(TspdfObj *page_dict, TspdfObj *new_resources,
                                TspdfArena *arena, TspdfRenameMap *renames);
uint8_t *tspdf_content_rewrite(const uint8_t *stream, size_t len,
                               const TspdfRenameMap *renames, size_t *out_len,
                               TspdfArena *arena);

// --- Stream filter decoding ---

// Run a stream's /Filter chain over raw (already decrypted) bytes. Returns a
// malloc'd buffer the caller frees. Defined in tspr_xref.c, used by the text
// extractor.
TspdfError tspdf_stream_decode(TspdfObj *stream_dict, const uint8_t *raw_data,
                               size_t raw_len, uint8_t **out_data, size_t *out_len);

// --- Lossy image recompression internals (tspr_lossy.c) ---

// Box-filter downsample (arbitrary ratios >= 1, per channel, exact edge
// weights). src is sw*sh*comps interleaved 8-bit pixels, dst dw*dh*comps.
// dw/dh must not exceed sw/sh (this never upsamples). Returns false on bad
// arguments or allocation failure, leaving dst untouched.
bool tspdf_lossy_box_downsample(const uint8_t *src, uint32_t sw, uint32_t sh,
                                int comps, uint8_t *dst, uint32_t dw, uint32_t dh);

// Bilevel box downsample: src is sw*sh bytes of 0/255 pixels; each dst
// pixel is the box average of its source window re-thresholded at 50%,
// with ties going to black so thin strokes keep their weight. Windows use
// integer floor boundaries (x0 = x*sw/dw, x1 = (x+1)*sw/dw), which
// partition the source exactly. Never upsamples. Returns false on bad
// arguments.
bool tspdf_lossy_bilevel_downsample(const uint8_t *src, uint32_t sw, uint32_t sh,
                                    uint8_t *dst, uint32_t dw, uint32_t dh);

// True when every RGB pixel has max(|R-G|,|R-B|,|G-B|) <= 8 (all pixels are
// checked; no sampling).
bool tspdf_lossy_rgb_is_gray(const uint8_t *rgb, size_t npix);

// BT.601-style integer luma: (77R + 150G + 29B + 128) >> 8.
void tspdf_lossy_rgb_to_gray(const uint8_t *rgb, size_t npix, uint8_t *out);

// Decide the downsample target for a w*h image rendered at w_pt x h_pt
// points. Returns true (and the target pixel size) only when the image has
// more than 1.3x the pixels needed at target_dpi; never upsamples.
bool tspdf_lossy_target_dims(uint32_t w, uint32_t h, double w_pt, double h_pt,
                             int target_dpi, uint32_t *dw, uint32_t *dh);

// Keep-the-original rule: replace only when the new stored stream is at
// least 10% smaller than the old one.
bool tspdf_lossy_worth_replacing(size_t old_stored, size_t new_stored);

// One drawn image XObject with its largest rendered size in points (the
// placement needing the most pixels), from walking every page's content
// streams (CTM tracking through q/Q/cm, recursing into Form XObjects).
typedef struct {
    uint32_t obj_num;
    double w_pt, h_pt;
} TspdfLossyPlacement;

// Scan all pages for image XObject placements. *out is malloc'd (caller
// frees); *count 0 with *out NULL when no image is drawn. Images with any
// unmeasurable placement (q/Q nesting too deep to track) are excluded.
TspdfError tspdf_lossy_scan_placements(TspdfReader *doc,
                                       TspdfLossyPlacement **out, size_t *count);

// --- New object registration ---

uint32_t tspdf_register_new_obj(TspdfReader *doc, TspdfObj *obj);

// --- Helpers ---

// Dict lookup helper: returns NULL if not found
TspdfObj *tspdf_dict_get(TspdfObj *dict, const char *key);

// Deep-copy an object into a target arena
TspdfObj *tspdf_obj_deep_copy(TspdfObj *obj, TspdfArena *dst);

// Bracket a page's /Contents with new streams: [prefix, ...old, suffix].
// Either side may be NULL to skip it. Used by page scaling to wrap the
// existing content in a "q <cm> ... Q" transform. Defined in tspr_content.c.
TspdfError tspdf_page_wrap_content(TspdfReader *doc, size_t page_index,
                                   const uint8_t *prefix, size_t prefix_len,
                                   const uint8_t *suffix, size_t suffix_len);

#endif
