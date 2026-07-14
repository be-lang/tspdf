// N-up imposition: place multiple source pages onto larger output sheets in a
// grid (2-up, 4-up, ...). This is the classic pdfjam/pdftk imposition feature.
//
// Approach (reusing existing machinery): the output is built as a TspdfReader,
// because tspdf_reader_import_page_xobject, tspdf_page_add_xobject, the content
// overlay path (tspdf_page_begin_content/end_content) and the serializer all
// operate on readers. We seed the output reader from the source's structure
// (sharing its data buffer, xref and a stripped catalog copy) so that object
// numbering works — new objects are numbered from src->xref.count upward — then
//
// LIFETIME: the returned document is NOT self-contained. It references the
// source's trailer, xref and backing data until it is serialized. The output
// holds an internal reference on the source (tspdf_reader_hold_source), so
// destroying the source before the output is saved is defended — the source's
// free is deferred until the output is destroyed. The tspdf CLI still observes
// the natural order (nup -> save -> destroy source).
//
// REPLACE the page list with freshly registered blank sheet page dicts. Each
// selected source page is imported as a self-contained /Form XObject (which
// decrypts encrypted sources and bounds hostile resource cycles) and placed in
// its grid cell with a uniform-scale `cm` matrix. Only page content is carried
// over: bookmarks, forms and annotations are dropped (this is imposition).
// An encrypted source's crypt is cloned into the output, so the save
// re-encrypts it with the original passwords like every other derived doc.

#include "tspr_internal.h"
#include "tspr_overlay.h"
#include "../pdf/pdf_stream.h"
#include <stdlib.h>
#include <string.h>

// Standard sheet sizes (points).
#define NUP_A4_W     595.276
#define NUP_A4_H     841.890
#define NUP_LETTER_W 612.0
#define NUP_LETTER_H 792.0

// Grid layout for a given cell count. cols x rows, reading order is
// left-to-right, top-to-bottom.
static bool nup_grid(unsigned n, unsigned *cols, unsigned *rows) {
    switch (n) {
        case 2:  *cols = 1; *rows = 2; return true;  // 1x2 (portrait); landscape swaps
        case 4:  *cols = 2; *rows = 2; return true;
        case 6:  *cols = 2; *rows = 3; return true;
        case 8:  *cols = 2; *rows = 4; return true;
        case 9:  *cols = 3; *rows = 3; return true;
        case 16: *cols = 4; *rows = 4; return true;
        default: return false;
    }
}

// --- Small arena object constructors ---

static TspdfObj *nup_obj(TspdfArena *a, TspdfObjType t) {
    TspdfObj *o = tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = t;
    return o;
}

static TspdfObj *nup_name(TspdfArena *a, const char *s) {
    TspdfObj *o = nup_obj(a, TSPDF_OBJ_NAME);
    if (!o) return NULL;
    size_t len = strlen(s);
    o->string.data = tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    memcpy(o->string.data, s, len + 1);
    o->string.len = len;
    return o;
}

static TspdfObj *nup_real(TspdfArena *a, double v) {
    TspdfObj *o = nup_obj(a, TSPDF_OBJ_REAL);
    if (o) o->real = v;
    return o;
}

static TspdfObj *nup_dict(TspdfArena *a, size_t cap) {
    TspdfObj *o = nup_obj(a, TSPDF_OBJ_DICT);
    if (!o) return NULL;
    o->dict.entries = tspdf_arena_alloc_zero(a, cap * sizeof(TspdfDictEntry));
    if (!o->dict.entries) return NULL;
    o->dict.count = 0;
    return o;
}

static bool nup_dict_add(TspdfObj *dict, TspdfArena *a, const char *key, TspdfObj *val) {
    if (!val) return false;
    size_t klen = strlen(key);
    char *k = tspdf_arena_alloc(a, klen + 1);
    if (!k) return false;
    memcpy(k, key, klen + 1);
    dict->dict.entries[dict->dict.count].key = k;
    dict->dict.entries[dict->dict.count].value = val;
    dict->dict.count++;
    return true;
}

// Build a blank page dict of the given size and register it as a new object in
// `doc`. Returns the object number, or 0 on failure. The page has an empty
// direct /Resources and an empty (registered) /Contents stream so the content
// overlay path has something to append to.
static uint32_t nup_add_blank_page(TspdfReader *doc, double w, double h) {
    TspdfArena *a = &doc->arena;

    // MediaBox [0 0 w h]
    TspdfObj *mbox = nup_obj(a, TSPDF_OBJ_ARRAY);
    if (!mbox) return 0;
    mbox->array.items = tspdf_arena_alloc_zero(a, 4 * sizeof(TspdfObj));
    if (!mbox->array.items) return 0;
    mbox->array.count = 4;
    double coords[4] = {0.0, 0.0, w, h};
    for (int i = 0; i < 4; i++) {
        TspdfObj *v = nup_real(a, coords[i]);
        if (!v) return 0;
        mbox->array.items[i] = *v;
    }

    // Empty content stream object.
    TspdfObj *cstream = nup_obj(a, TSPDF_OBJ_STREAM);
    if (!cstream) return 0;
    TspdfObj *cdict = nup_dict(a, 2);
    if (!cdict) return 0;
    TspdfObj *len0 = nup_obj(a, TSPDF_OBJ_INT);
    if (!len0) return 0;
    len0->integer = 0;
    if (!nup_dict_add(cdict, a, "Length", len0)) return 0;
    cstream->stream.dict = cdict;
    cstream->stream.data = tspdf_arena_alloc(a, 1);
    if (!cstream->stream.data) return 0;
    cstream->stream.len = 0;
    cstream->stream.raw_offset = 0;
    cstream->stream.raw_len = 0;
    cstream->stream.self_contained = true;
    uint32_t cnum = tspdf_register_new_obj(doc, cstream);
    if (cnum == 0) return 0;

    TspdfObj *cref = nup_obj(a, TSPDF_OBJ_REF);
    if (!cref) return 0;
    cref->ref.num = cnum;
    cref->ref.gen = 0;

    // Page dict: << /Type /Page /MediaBox ... /Resources << >> /Contents cnum >>
    TspdfObj *page = nup_dict(a, 4);
    if (!page) return 0;
    TspdfObj *res = nup_dict(a, 1);
    if (!res) return 0;
    if (!nup_dict_add(page, a, "Type", nup_name(a, "Page"))) return 0;
    if (!nup_dict_add(page, a, "MediaBox", mbox)) return 0;
    if (!nup_dict_add(page, a, "Resources", res)) return 0;
    if (!nup_dict_add(page, a, "Contents", cref)) return 0;

    return tspdf_register_new_obj(doc, page);
}

// Give the output doc a minimal own catalog so the serializer does not fall
// back to the source catalog (which would drag in the source page tree,
// outlines, forms, etc.). << /Type /Catalog >> is enough: the serializer
// synthesizes /Pages as object 2.
static bool nup_set_catalog(TspdfReader *doc) {
    TspdfArena *a = &doc->arena;
    TspdfObj *cat = nup_dict(a, 2);
    if (!cat) return false;
    if (!nup_dict_add(cat, a, "Type", nup_name(a, "Catalog"))) return false;
    doc->catalog = cat;
    return true;
}

TspdfReader *tspdf_reader_nup(TspdfReader *src, const TspdfNupOptions *opts,
                              TspdfError *err) {
    if (err) *err = TSPDF_OK;
    if (!src || !opts) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }

    unsigned cols = 0, rows = 0;
    if (!nup_grid(opts->n, &cols, &rows)) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    if (opts->gap < 0.0 || opts->gap != opts->gap) {  // negative or NaN
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }

    // Resolve the selection: explicit list, or all source pages in order.
    size_t src_count = src->pages.count;
    const size_t *sel = opts->pages;
    size_t sel_count = opts->pages ? opts->page_count : src_count;
    if (opts->pages && sel_count == 0) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    if (sel_count == 0) {
        if (err) *err = TSPDF_ERR_INVALID_ARG;
        return NULL;
    }
    for (size_t i = 0; i < sel_count; i++) {
        size_t idx = sel ? sel[i] : i;
        if (idx >= src_count) {
            if (err) *err = TSPDF_ERR_PAGE_RANGE;
            return NULL;
        }
    }

    // Determine the output sheet size (before landscape swap).
    double sheet_w = NUP_A4_W, sheet_h = NUP_A4_H;
    if (opts->size == TSPDF_NUP_SIZE_LETTER) {
        sheet_w = NUP_LETTER_W;
        sheet_h = NUP_LETTER_H;
    } else if (opts->size == TSPDF_NUP_SIZE_SOURCE) {
        // Size the sheet as the grid of the first selected source page's box.
        size_t first = sel ? sel[0] : 0;
        TspdfReaderPage *p0 = &src->pages.pages[first];
        double pw = p0->media_box[2] - p0->media_box[0];
        double ph = p0->media_box[3] - p0->media_box[1];
        int rot0 = ((p0->rotate % 360) + 360) % 360;
        double vw = (rot0 == 90 || rot0 == 270) ? ph : pw;
        double vh = (rot0 == 90 || rot0 == 270) ? pw : ph;
        if (vw <= 0 || vh <= 0) { vw = NUP_A4_W; vh = NUP_A4_H; }
        sheet_w = vw * (double)cols;
        sheet_h = vh * (double)rows;
    }
    if (opts->landscape) {
        double t = sheet_w; sheet_w = sheet_h; sheet_h = t;
    }

    // --- Build the output reader, sharing the source's backing structure ---
    TspdfReader *out = (TspdfReader *)calloc(1, sizeof(TspdfReader));
    if (!out) {
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    size_t arena_cap = src->arena.total_allocated;
    if (arena_cap < 65536) arena_cap = 65536;
    out->arena = tspdf_arena_create(arena_cap);
    if (!out->arena.first) {
        free(out);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Share the source buffer and xref so object resolution during import and
    // serialization works. The output owns none of it; the source must outlive
    // the output only until it is saved (imports are self-contained).
    out->data = src->data;
    out->data_len = src->data_len;
    out->owns_data = false;
    memcpy(out->pdf_version, src->pdf_version, sizeof(out->pdf_version));
    if (strcmp(out->pdf_version, "1.4") < 0) memcpy(out->pdf_version, "1.4", 4);

    out->xref.count = src->xref.count;
    out->xref.entries = src->xref.entries;
    out->xref.trailer = src->xref.trailer;

    // Own obj_cache, left entirely NULL: the output's page tree references only
    // imported (new) objects, so no source object is ever resolved through
    // out->obj_cache. Crucially, NOT aliasing the source's cache means the
    // output's free_stream_data never frees stream bytes the source owns (which
    // import may have materialized) — no double free.
    out->obj_cache = (TspdfObj **)tspdf_arena_alloc_zero(&out->arena,
                        sizeof(TspdfObj *) * (src->xref.count ? src->xref.count : 1));
    if (!out->obj_cache) {
        tspdf_arena_destroy(&out->arena);
        free(out);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    if (!nup_set_catalog(out)) {
        tspdf_arena_destroy(&out->arena);
        free(out);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }

    // Number of output sheets.
    unsigned per_sheet = cols * rows;
    size_t sheets = (sel_count + per_sheet - 1) / per_sheet;

    out->pages.pages = (TspdfReaderPage *)tspdf_arena_alloc_zero(&out->arena,
                            sizeof(TspdfReaderPage) * sheets);
    if (!out->pages.pages) {
        tspdf_arena_destroy(&out->arena);
        free(out);
        if (err) *err = TSPDF_ERR_ALLOC;
        return NULL;
    }
    out->pages.count = 0;  // grown as blank pages are registered

    // Create the blank sheets.
    for (size_t s = 0; s < sheets; s++) {
        uint32_t pnum = nup_add_blank_page(out, sheet_w, sheet_h);
        if (pnum == 0) {
            tspdf_reader_destroy(out);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        TspdfReaderPage *pg = &out->pages.pages[out->pages.count];
        pg->obj_num = pnum;
        // The registered page dict is the last new object.
        pg->page_dict = out->new_objs.objs[out->new_objs.count - 1];
        pg->media_box[0] = 0.0;
        pg->media_box[1] = 0.0;
        pg->media_box[2] = sheet_w;
        pg->media_box[3] = sheet_h;
        pg->user_unit = 1.0;
        pg->rotate = 0;
        out->pages.count++;
    }

    // Cell geometry (points). Cells are laid out with `gap` between and around
    // them so the grid is centered on the sheet.
    double gap = opts->gap;
    double cell_w = (sheet_w - gap * (double)(cols + 1)) / (double)cols;
    double cell_h = (sheet_h - gap * (double)(rows + 1)) / (double)rows;
    if (cell_w <= 0 || cell_h <= 0) {
        // Gap too large for the sheet: fall back to no gap.
        gap = 0.0;
        cell_w = sheet_w / (double)cols;
        cell_h = sheet_h / (double)rows;
    }

    // Place each selected source page.
    for (size_t i = 0; i < sel_count; i++) {
        size_t src_idx = sel ? sel[i] : i;
        size_t sheet = i / per_sheet;
        unsigned cell = (unsigned)(i % per_sheet);
        unsigned col = cell % cols;
        unsigned row = cell / cols;  // 0 = top row

        double bbox[4] = {0};
        TspdfError ierr = TSPDF_OK;
        uint32_t xnum = tspdf_reader_import_page_xobject(out, src, src_idx, bbox, &ierr);
        if (xnum == 0) {
            tspdf_reader_destroy(out);
            if (err) *err = ierr;
            return NULL;
        }

        double bw = bbox[2] - bbox[0];
        double bh = bbox[3] - bbox[1];
        if (bw <= 0 || bh <= 0) continue;

        // Cell origin (bottom-left). Rows count from the top.
        double cell_x = gap + (double)col * (cell_w + gap);
        double cell_y_top = sheet_h - (gap + (double)row * (cell_h + gap));
        double cell_y = cell_y_top - cell_h;

        // Uniform scale to fit the cell.
        double sx = cell_w / bw;
        double sy = cell_h / bh;
        double scale = sx < sy ? sx : sy;
        if (!(scale > 0)) continue;

        // Center the scaled page within the cell.
        double drawn_w = bw * scale;
        double drawn_h = bh * scale;
        double ox = cell_x + (cell_w - drawn_w) / 2.0;
        double oy = cell_y + (cell_h - drawn_h) / 2.0;

        const char *name = tspdf_page_add_xobject(out, sheet, xnum);
        if (!name) {
            tspdf_reader_destroy(out);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }

        TspdfStream *cs = tspdf_page_begin_content(out, sheet);
        if (!cs) {
            tspdf_reader_destroy(out);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }

        // Optional cell frame (thin stroked rect around the cell).
        if (opts->frame) {
            tspdf_stream_save(cs);
            tspdf_stream_set_line_width(cs, 0.5);
            tspdf_stream_rect(cs, cell_x, cell_y, cell_w, cell_h);
            tspdf_stream_stroke(cs);
            tspdf_stream_restore(cs);
        }

        // Place the imported page. draw_image emits [a 0 0 d x y] cm /name Do;
        // pass the uniform scale as a and d (NOT the drawn size — the form's
        // content is in BBox units), and offset by -bbox[0]*scale so a
        // non-zero BBox origin lands at the cell origin.
        tspdf_stream_draw_image(cs, name,
                                ox - bbox[0] * scale,
                                oy - bbox[1] * scale,
                                scale, scale);

        TspdfError eerr = tspdf_page_end_content(out, sheet, cs, NULL);
        if (eerr != TSPDF_OK) {
            tspdf_reader_destroy(out);
            if (err) *err = eerr;
            return NULL;
        }
    }

    // An encrypted source stays encrypted: carry its crypt into the output so
    // the save re-encrypts with the recovered file key and preserved /Encrypt
    // dict (the same mechanism extract/delete/rotate use). The clone points
    // into the source's arena, which the lifetime rule above already covers.
    // Set last so no build step above consults it — the imported objects are
    // self-contained plaintext, encrypted only as they are written out.
    if (src->crypt) {
        out->crypt = tspdf_crypt_clone(src->crypt);
        if (!out->crypt) {
            tspdf_reader_destroy(out);
            if (err) *err = TSPDF_ERR_ALLOC;
            return NULL;
        }
    }

    // The output aliases src's data/xref/trailer (and crypt when cloned above):
    // hold a reference so destroying src before this doc is saved defers the
    // free. Set last, on the fully-built reader — every failure path above
    // destroys `out` before it holds any reference.
    tspdf_reader_hold_source(out, src);

    out->modified = true;
    return out;
}
