// AcroForm form fields: enumerate, fill, flatten (feat/form).
//
// All of this walks attacker-controlled structure (the field tree hangs off
// the catalog), so every recursion carries the same guards as tspr_doctree.c:
// a depth cap plus a shared node budget, because cyclic /Kids fan out to
// count^depth visits under a depth cap alone.

#include "tspr_internal.h"
#include "tspr_overlay.h"
#include "../util/pdftext.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FORM_MAX_DEPTH 32

// --- small helpers ---

// Length-checked name compare (parsed names can carry embedded NULs).
static bool form_name_is(const TspdfObj *o, const char *name) {
    size_t len = strlen(name);
    return o && o->type == TSPDF_OBJ_NAME && o->string.len == len &&
           memcmp(o->string.data, name, len) == 0;
}

static double form_number(const TspdfObj *o, double fallback) {
    if (!o) return fallback;
    if (o->type == TSPDF_OBJ_INT) return (double)o->integer;
    if (o->type == TSPDF_OBJ_REAL) return o->real;
    return fallback;
}

// --- resolution (same contract as dt_resolve_num in tspr_doctree.c) ---

static TspdfObj *form_resolve_num(TspdfReader *doc, TspdfParser *parser,
                                  uint32_t num) {
    if (num < doc->xref.count) {
        if (doc->obj_cache[num]) return doc->obj_cache[num];
        if (!doc->data) return NULL;
        return tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache,
                                  doc->crypt);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
}

static TspdfObj *form_resolve(TspdfReader *doc, TspdfParser *parser,
                              TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_REF) return obj;
    return form_resolve_num(doc, parser, obj->ref.num);
}

// The document catalog (opened documents carry it; fall back to trailer Root).
static TspdfObj *form_catalog(TspdfReader *doc, TspdfParser *parser) {
    if (doc->catalog && doc->catalog->type == TSPDF_OBJ_DICT) return doc->catalog;
    if (!doc->xref.trailer) return NULL;
    TspdfObj *root = form_resolve(doc, parser,
                                  tspdf_dict_get(doc->xref.trailer, "Root"));
    return root && root->type == TSPDF_OBJ_DICT ? root : NULL;
}

// --- text decoding (same policy as the metadata getters) ---

// PDF text string -> NUL-terminated UTF-8 in the arena: UTF-16BE with BOM is
// decoded, anything else is copied raw (PDFDocEncoding is ASCII-compatible
// for the printable range).
static char *form_decode_text(TspdfArena *a, const TspdfObj *s) {
    if (!s || (s->type != TSPDF_OBJ_STRING && s->type != TSPDF_OBJ_NAME)) {
        return NULL;
    }
    char *utf8 = tspdf_utf16be_to_utf8(s->string.data, s->string.len, a);
    if (utf8) return utf8;
    char *out = (char *)tspdf_arena_alloc(a, s->string.len + 1);
    if (!out) return NULL;
    if (s->string.len > 0) memcpy(out, s->string.data, s->string.len);
    out[s->string.len] = '\0';
    return out;
}

// --- terminal field collection ---
//
// A terminal field is a node none of whose /Kids carries /T: kids without /T
// are widget annotations of the field itself, and a field without /Kids is
// its own (merged) widget. Interior nodes only contribute their /T to the
// qualified name and their inheritable entries (/FT /V /DA /Ff).

typedef struct {
    const char *name;       // qualified dotted name (arena), "" when unnamed
    TspdfObj *field;        // terminal field dict (in the object cache)
    uint32_t field_num;     // object number, 0 when inline/unknown
    TspdfObj **widgets;     // widget dicts (arena array)
    uint32_t *widget_nums;  // object numbers (0 = unknown)
    size_t widget_count;
    TspdfFieldType type;
    TspdfObj *v;            // effective /V, resolved (may be NULL)
    TspdfObj *da;           // effective /DA string (field else AcroForm)
    int64_t ff;             // effective /Ff
} FormTerminal;

typedef struct {
    TspdfReader *doc;
    TspdfParser parser;
    TspdfArena *a;
    size_t budget;          // shared node budget across the whole walk
    FormTerminal *items;    // malloc'd; freed by form_terminals_free
    size_t count;
    size_t cap;
    TspdfObj *acroform;     // resolved AcroForm dict (may be inline in catalog)
    TspdfError err;
} FormCtx;

static void form_terminals_free(FormCtx *ctx) {
    free(ctx->items);
    ctx->items = NULL;
    ctx->count = ctx->cap = 0;
}

static FormTerminal *form_push_terminal(FormCtx *ctx) {
    if (ctx->count >= ctx->cap) {
        size_t cap = ctx->cap == 0 ? 16 : ctx->cap * 2;
        FormTerminal *grown =
            (FormTerminal *)realloc(ctx->items, cap * sizeof(FormTerminal));
        if (!grown) {
            ctx->err = TSPDF_ERR_ALLOC;
            return NULL;
        }
        ctx->items = grown;
        ctx->cap = cap;
    }
    FormTerminal *t = &ctx->items[ctx->count++];
    memset(t, 0, sizeof(*t));
    return t;
}

static TspdfFieldType form_field_type(TspdfObj *ft, int64_t ff) {
    if (form_name_is(ft, "Tx")) return TSPDF_FIELD_TEXT;
    if (form_name_is(ft, "Ch")) return TSPDF_FIELD_CHOICE;
    if (form_name_is(ft, "Sig")) return TSPDF_FIELD_SIGNATURE;
    if (form_name_is(ft, "Btn")) {
        if (ff & (1 << 16)) return TSPDF_FIELD_PUSHBUTTON;
        if (ff & (1 << 15)) return TSPDF_FIELD_RADIO;
        return TSPDF_FIELD_CHECKBOX;
    }
    return TSPDF_FIELD_UNKNOWN;
}

static void form_walk(FormCtx *ctx, TspdfObj *field_val, const char *prefix,
                      TspdfObj *in_ft, TspdfObj *in_v, TspdfObj *in_da,
                      int64_t in_ff, int depth) {
    if (ctx->err != TSPDF_OK || depth > FORM_MAX_DEPTH) return;
    if (ctx->budget == 0) return;
    ctx->budget--;

    uint32_t num = 0;
    if (field_val && field_val->type == TSPDF_OBJ_REF) num = field_val->ref.num;
    TspdfObj *field = form_resolve(ctx->doc, &ctx->parser, field_val);
    if (!field || field->type != TSPDF_OBJ_DICT) return;

    // Qualified name: parent chain of /T joined with '.'.
    const char *name = prefix;
    TspdfObj *t = form_resolve(ctx->doc, &ctx->parser,
                               tspdf_dict_get(field, "T"));
    if (t && t->type == TSPDF_OBJ_STRING) {
        char *part = form_decode_text(ctx->a, t);
        if (!part) {
            ctx->err = TSPDF_ERR_ALLOC;
            return;
        }
        if (prefix && prefix[0]) {
            size_t plen = strlen(prefix);
            size_t slen = strlen(part);
            char *joined = (char *)tspdf_arena_alloc(ctx->a, plen + slen + 2);
            if (!joined) {
                ctx->err = TSPDF_ERR_ALLOC;
                return;
            }
            memcpy(joined, prefix, plen);
            joined[plen] = '.';
            memcpy(joined + plen + 1, part, slen + 1);
            name = joined;
        } else {
            name = part;
        }
    }

    // Inheritable entries: own value wins, else the inherited one.
    TspdfObj *ft = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(field, "FT"));
    if (!ft || ft->type != TSPDF_OBJ_NAME) ft = in_ft;
    TspdfObj *v = form_resolve(ctx->doc, &ctx->parser,
                               tspdf_dict_get(field, "V"));
    if (!v) v = in_v;
    TspdfObj *da = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(field, "DA"));
    if (!da || da->type != TSPDF_OBJ_STRING) da = in_da;
    TspdfObj *ffo = form_resolve(ctx->doc, &ctx->parser,
                                 tspdf_dict_get(field, "Ff"));
    int64_t ff = (ffo && ffo->type == TSPDF_OBJ_INT) ? ffo->integer : in_ff;

    TspdfObj *kids = form_resolve(ctx->doc, &ctx->parser,
                                  tspdf_dict_get(field, "Kids"));
    if (kids && kids->type != TSPDF_OBJ_ARRAY) kids = NULL;

    // A kid with /T is a child field -> this node is interior.
    bool has_child_fields = false;
    if (kids) {
        for (size_t i = 0; i < kids->array.count && !has_child_fields; i++) {
            TspdfObj *kid = form_resolve(ctx->doc, &ctx->parser,
                                         &kids->array.items[i]);
            if (kid && kid->type == TSPDF_OBJ_DICT && tspdf_dict_get(kid, "T")) {
                has_child_fields = true;
            }
        }
    }

    if (has_child_fields) {
        for (size_t i = 0; i < kids->array.count; i++) {
            form_walk(ctx, &kids->array.items[i], name, ft, v, da, ff,
                      depth + 1);
            if (ctx->err != TSPDF_OK) return;
        }
        return;
    }

    // Terminal field: widgets are the /Kids (all without /T), or the field
    // itself when it has none.
    FormTerminal *term = form_push_terminal(ctx);
    if (!term) return;
    term->name = (name && name[0]) ? name : "";
    term->field = field;
    term->field_num = num;
    term->v = v;
    term->da = da;
    term->ff = ff;
    term->type = form_field_type(ft, ff);

    size_t kid_count = kids ? kids->array.count : 0;
    size_t max_widgets = kid_count > 0 ? kid_count : 1;
    term->widgets = (TspdfObj **)tspdf_arena_alloc_zero(
        ctx->a, max_widgets * sizeof(TspdfObj *));
    term->widget_nums = (uint32_t *)tspdf_arena_alloc_zero(
        ctx->a, max_widgets * sizeof(uint32_t));
    if (!term->widgets || !term->widget_nums) {
        ctx->err = TSPDF_ERR_ALLOC;
        return;
    }
    if (kid_count > 0) {
        for (size_t i = 0; i < kid_count; i++) {
            if (ctx->budget == 0) break;
            ctx->budget--;
            TspdfObj *kid_val = &kids->array.items[i];
            TspdfObj *kid = form_resolve(ctx->doc, &ctx->parser, kid_val);
            if (!kid || kid->type != TSPDF_OBJ_DICT) continue;
            term->widgets[term->widget_count] = kid;
            term->widget_nums[term->widget_count] =
                kid_val->type == TSPDF_OBJ_REF ? kid_val->ref.num : 0;
            term->widget_count++;
        }
    } else {
        term->widgets[0] = field;
        term->widget_nums[0] = num;
        term->widget_count = 1;
    }
}

// Collect all terminal fields. On success the caller must free the items
// with form_terminals_free (the pointed-to objects live in the arena).
static TspdfError form_collect_terminals(TspdfReader *doc, FormCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->doc = doc;
    ctx->a = &doc->arena;
    ctx->err = TSPDF_OK;
    tspdf_parser_init(&ctx->parser, doc->data, doc->data_len, &doc->arena);
    // A legitimate tree visits each object at most once.
    ctx->budget = doc->xref.count + doc->new_objs.count + 16;

    TspdfObj *catalog = form_catalog(doc, &ctx->parser);
    if (!catalog) return TSPDF_OK;
    TspdfObj *acro = form_resolve(doc, &ctx->parser,
                                  tspdf_dict_get(catalog, "AcroForm"));
    if (!acro || acro->type != TSPDF_OBJ_DICT) return TSPDF_OK;
    ctx->acroform = acro;

    TspdfObj *acro_da = form_resolve(doc, &ctx->parser,
                                     tspdf_dict_get(acro, "DA"));
    if (acro_da && acro_da->type != TSPDF_OBJ_STRING) acro_da = NULL;

    TspdfObj *fields = form_resolve(doc, &ctx->parser,
                                    tspdf_dict_get(acro, "Fields"));
    if (!fields || fields->type != TSPDF_OBJ_ARRAY) return TSPDF_OK;

    for (size_t i = 0; i < fields->array.count; i++) {
        form_walk(ctx, &fields->array.items[i], "", NULL, NULL, acro_da, 0, 0);
        if (ctx->err != TSPDF_OK) break;
    }
    if (ctx->err != TSPDF_OK) {
        form_terminals_free(ctx);
        return ctx->err;
    }
    return TSPDF_OK;
}

// --- widget -> page mapping ---

// Map object number -> page index + 1 (0 = unknown) for every annotation
// referenced from a page /Annots array. malloc'd; caller frees.
static size_t *form_build_annot_page_map(TspdfReader *doc, TspdfParser *parser,
                                         size_t *out_size) {
    size_t size = doc->xref.count + doc->new_objs.count;
    *out_size = size;
    if (size == 0) return NULL;
    size_t *map = (size_t *)calloc(size, sizeof(size_t));
    if (!map) return NULL;
    for (size_t p = 0; p < doc->pages.count; p++) {
        TspdfObj *annots = form_resolve(doc, parser,
            tspdf_dict_get(doc->pages.pages[p].page_dict, "Annots"));
        if (!annots || annots->type != TSPDF_OBJ_ARRAY) continue;
        for (size_t i = 0; i < annots->array.count; i++) {
            TspdfObj *ref = &annots->array.items[i];
            if (ref->type == TSPDF_OBJ_REF && ref->ref.num < size &&
                map[ref->ref.num] == 0) {
                map[ref->ref.num] = p + 1;
            }
        }
    }
    return map;
}

// Page index of a widget: /P first, then the /Annots map. SIZE_MAX unknown.
static size_t form_widget_page(TspdfReader *doc, TspdfParser *parser,
                               TspdfObj *widget, uint32_t widget_num,
                               const size_t *annot_map, size_t map_size) {
    (void)parser;
    TspdfObj *p = widget ? tspdf_dict_get(widget, "P") : NULL;
    if (p && p->type == TSPDF_OBJ_REF) {
        for (size_t i = 0; i < doc->pages.count; i++) {
            if (doc->pages.pages[i].obj_num == p->ref.num) return i;
        }
    }
    if (annot_map && widget_num > 0 && widget_num < map_size &&
        annot_map[widget_num] > 0) {
        return annot_map[widget_num] - 1;
    }
    return (size_t)-1;
}

// --- option collection ---

static bool form_option_seen(const char **options, size_t count,
                             const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (options[i] && strcmp(options[i], name) == 0) return true;
    }
    return false;
}

// Button "on" states: keys of every widget's /AP /N dict except /Off.
static TspdfError form_button_options(FormCtx *ctx, const FormTerminal *term,
                                      const char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    size_t cap = 0;
    for (size_t w = 0; w < term->widget_count; w++) {
        TspdfObj *ap = form_resolve(ctx->doc, &ctx->parser,
                                    tspdf_dict_get(term->widgets[w], "AP"));
        if (!ap || ap->type != TSPDF_OBJ_DICT) continue;
        TspdfObj *n = form_resolve(ctx->doc, &ctx->parser,
                                   tspdf_dict_get(ap, "N"));
        if (!n || n->type != TSPDF_OBJ_DICT) continue;
        cap += n->dict.count;
    }
    if (cap == 0) return TSPDF_OK;
    const char **options =
        (const char **)tspdf_arena_alloc_zero(ctx->a, cap * sizeof(char *));
    if (!options) return TSPDF_ERR_ALLOC;
    size_t count = 0;
    for (size_t w = 0; w < term->widget_count; w++) {
        TspdfObj *ap = form_resolve(ctx->doc, &ctx->parser,
                                    tspdf_dict_get(term->widgets[w], "AP"));
        if (!ap || ap->type != TSPDF_OBJ_DICT) continue;
        TspdfObj *n = form_resolve(ctx->doc, &ctx->parser,
                                   tspdf_dict_get(ap, "N"));
        if (!n || n->type != TSPDF_OBJ_DICT) continue;
        for (size_t i = 0; i < n->dict.count && count < cap; i++) {
            const char *key = n->dict.entries[i].key;
            if (!key || strcmp(key, "Off") == 0) continue;
            if (form_option_seen(options, count, key)) continue;
            options[count++] = key;
        }
    }
    *out = options;
    *out_count = count;
    return TSPDF_OK;
}

// Choice options: /Opt export values (first element of two-element arrays).
static TspdfError form_choice_options(FormCtx *ctx, const FormTerminal *term,
                                      const char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    TspdfObj *opt = form_resolve(ctx->doc, &ctx->parser,
                                 tspdf_dict_get(term->field, "Opt"));
    if (!opt || opt->type != TSPDF_OBJ_ARRAY || opt->array.count == 0) {
        return TSPDF_OK;
    }
    const char **options = (const char **)tspdf_arena_alloc_zero(
        ctx->a, opt->array.count * sizeof(char *));
    if (!options) return TSPDF_ERR_ALLOC;
    size_t count = 0;
    for (size_t i = 0; i < opt->array.count; i++) {
        TspdfObj *item = form_resolve(ctx->doc, &ctx->parser,
                                      &opt->array.items[i]);
        if (item && item->type == TSPDF_OBJ_ARRAY && item->array.count > 0) {
            item = form_resolve(ctx->doc, &ctx->parser, &item->array.items[0]);
        }
        if (!item || item->type != TSPDF_OBJ_STRING) continue;
        char *text = form_decode_text(ctx->a, item);
        if (!text) return TSPDF_ERR_ALLOC;
        options[count++] = text;
    }
    *out = options;
    *out_count = count;
    return TSPDF_OK;
}

// --- public: enumerate ---

TspdfError tspdf_reader_form_fields(TspdfReader *doc,
                                    TspdfFormFieldInfo **out_fields,
                                    size_t *out_count) {
    if (!doc || !out_fields || !out_count) return TSPDF_ERR_INVALID_ARG;
    *out_fields = NULL;
    *out_count = 0;

    FormCtx ctx;
    TspdfError err = form_collect_terminals(doc, &ctx);
    if (err != TSPDF_OK) return err;
    if (ctx.count == 0) {
        form_terminals_free(&ctx);
        return TSPDF_OK;
    }

    TspdfFormFieldInfo *infos = (TspdfFormFieldInfo *)tspdf_arena_alloc_zero(
        &doc->arena, ctx.count * sizeof(TspdfFormFieldInfo));
    if (!infos) {
        form_terminals_free(&ctx);
        return TSPDF_ERR_ALLOC;
    }

    size_t map_size = 0;
    size_t *annot_map = form_build_annot_page_map(doc, &ctx.parser, &map_size);

    for (size_t i = 0; i < ctx.count && err == TSPDF_OK; i++) {
        const FormTerminal *term = &ctx.items[i];
        TspdfFormFieldInfo *info = &infos[i];
        info->name = term->name;
        info->type = term->type;
        info->readonly = (term->ff & 1) != 0;
        info->required = (term->ff & 2) != 0;
        info->page_index = (size_t)-1;

        if (term->v) {
            if (term->v->type == TSPDF_OBJ_STRING ||
                term->v->type == TSPDF_OBJ_NAME) {
                info->value = form_decode_text(&doc->arena, term->v);
                if (!info->value) err = TSPDF_ERR_ALLOC;
            }
        }

        // First widget provides page and rect.
        for (size_t w = 0; w < term->widget_count; w++) {
            size_t page = form_widget_page(doc, &ctx.parser, term->widgets[w],
                                           term->widget_nums[w], annot_map,
                                           map_size);
            if (page != (size_t)-1) {
                info->page_index = page;
                break;
            }
        }
        if (term->widget_count > 0 && err == TSPDF_OK) {
            TspdfObj *rect = form_resolve(doc, &ctx.parser,
                tspdf_dict_get(term->widgets[0], "Rect"));
            if (rect && rect->type == TSPDF_OBJ_ARRAY && rect->array.count == 4) {
                for (size_t k = 0; k < 4; k++) {
                    info->rect[k] = form_number(
                        form_resolve(doc, &ctx.parser, &rect->array.items[k]),
                        0.0);
                }
            }
        }

        if (err == TSPDF_OK) {
            if (info->type == TSPDF_FIELD_CHECKBOX ||
                info->type == TSPDF_FIELD_RADIO) {
                err = form_button_options(&ctx, term, &info->options,
                                          &info->option_count);
            } else if (info->type == TSPDF_FIELD_CHOICE) {
                err = form_choice_options(&ctx, term, &info->options,
                                          &info->option_count);
            }
        }
    }

    size_t count = ctx.count;
    free(annot_map);
    form_terminals_free(&ctx);
    if (err != TSPDF_OK) return err;
    *out_fields = infos;
    *out_count = count;
    return TSPDF_OK;
}

// --- object builders (all allocations in the document arena) ---

static TspdfObj *form_obj_new(TspdfArena *a, TspdfObjType type) {
    TspdfObj *o = (TspdfObj *)tspdf_arena_alloc_zero(a, sizeof(TspdfObj));
    if (o) o->type = type;
    return o;
}

static TspdfObj *form_make_name(TspdfArena *a, const char *name) {
    TspdfObj *o = form_obj_new(a, TSPDF_OBJ_NAME);
    if (!o) return NULL;
    size_t len = strlen(name);
    o->string.data = (uint8_t *)tspdf_arena_alloc(a, len + 1);
    if (!o->string.data) return NULL;
    memcpy(o->string.data, name, len + 1);
    o->string.len = len;
    return o;
}

static TspdfObj *form_make_bool(TspdfArena *a, bool v) {
    TspdfObj *o = form_obj_new(a, TSPDF_OBJ_BOOL);
    if (o) o->boolean = v;
    return o;
}

static TspdfObj *form_make_ref(TspdfArena *a, uint32_t num) {
    TspdfObj *o = form_obj_new(a, TSPDF_OBJ_REF);
    if (o) o->ref.num = num;
    return o;
}

static TspdfObj *form_make_int(TspdfArena *a, int64_t v) {
    TspdfObj *o = form_obj_new(a, TSPDF_OBJ_INT);
    if (o) o->integer = v;
    return o;
}

// Append or replace `key` in an arena-backed dict.
static TspdfError form_dict_put(TspdfObj *dict, const char *key,
                                TspdfObj *value, TspdfArena *a) {
    if (!dict || dict->type != TSPDF_OBJ_DICT || !value) return TSPDF_ERR_ALLOC;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            dict->dict.entries[i].value = value;
            return TSPDF_OK;
        }
    }
    TspdfDictEntry *entries = (TspdfDictEntry *)tspdf_arena_alloc(a,
        sizeof(TspdfDictEntry) * (dict->dict.count + 1));
    if (!entries) return TSPDF_ERR_ALLOC;
    if (dict->dict.count > 0) {
        memcpy(entries, dict->dict.entries,
               sizeof(TspdfDictEntry) * dict->dict.count);
    }
    size_t klen = strlen(key);
    char *kcopy = (char *)tspdf_arena_alloc(a, klen + 1);
    if (!kcopy) return TSPDF_ERR_ALLOC;
    memcpy(kcopy, key, klen + 1);
    entries[dict->dict.count].key = kcopy;
    entries[dict->dict.count].value = value;
    dict->dict.entries = entries;
    dict->dict.count++;
    return TSPDF_OK;
}

static void form_dict_remove(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) {
            memmove(&dict->dict.entries[i], &dict->dict.entries[i + 1],
                    sizeof(TspdfDictEntry) * (dict->dict.count - i - 1));
            dict->dict.count--;
            return;
        }
    }
}

// PDF text string object for a UTF-8 value: ASCII stays raw, anything else
// becomes a BOM-prefixed UTF-16BE string (ISO 32000 §7.9.2.2), matching the
// Info-dictionary convention. Invalid UTF-8 is stored raw.
static TspdfObj *form_make_text_string(TspdfArena *a, const char *utf8) {
    TspdfObj *o = form_obj_new(a, TSPDF_OBJ_STRING);
    if (!o) return NULL;
    size_t len = strlen(utf8);
    if (tspdf_str_is_ascii(utf8)) {
        o->string.data = (uint8_t *)tspdf_arena_alloc(a, len + 1);
        if (!o->string.data) return NULL;
        memcpy(o->string.data, utf8, len + 1);
        o->string.len = len;
        return o;
    }
    // Worst case: every UTF-8 byte becomes a surrogate pair unit.
    uint8_t *buf = (uint8_t *)tspdf_arena_alloc(a, 2 + len * 4 + 1);
    if (!buf) return NULL;
    size_t pos = 0;
    buf[pos++] = 0xFE;
    buf[pos++] = 0xFF;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = 0;
        size_t consumed = tspdf_utf8_decode(p, &cp);
        if (consumed == 0) {
            // Not valid UTF-8 after all: store the raw bytes unchanged.
            uint8_t *raw = (uint8_t *)tspdf_arena_alloc(a, len + 1);
            if (!raw) return NULL;
            memcpy(raw, utf8, len + 1);
            o->string.data = raw;
            o->string.len = len;
            return o;
        }
        p += consumed;
        if (cp > 0xFFFF) {
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 + (v >> 10);
            uint32_t lo = 0xDC00 + (v & 0x3FF);
            buf[pos++] = (uint8_t)(hi >> 8);
            buf[pos++] = (uint8_t)hi;
            buf[pos++] = (uint8_t)(lo >> 8);
            buf[pos++] = (uint8_t)lo;
        } else {
            buf[pos++] = (uint8_t)(cp >> 8);
            buf[pos++] = (uint8_t)cp;
        }
    }
    o->string.data = buf;
    o->string.len = pos;
    return o;
}

// --- default appearance (/DA) parsing ---

// Extract "<font> <size> Tf" from a DA string ("0 0 0 rg /Helv 0 Tf" and
// friends). Defaults: font "Helv", size 0 (= auto).
static void form_parse_da(const TspdfObj *da, char *font, size_t font_sz,
                          double *size) {
    snprintf(font, font_sz, "Helv");
    *size = 0;
    if (!da || da->type != TSPDF_OBJ_STRING || !da->string.data) return;

    const uint8_t *s = da->string.data;
    size_t len = da->string.len;
    char tok[2][64] = {{0}, {0}};  // previous two tokens
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
                           s[i] == '\n')) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
               s[i] != '\n') i++;
        size_t tlen = i - start;
        if (tlen == 2 && s[start] == 'T' && s[start + 1] == 'f') {
            // tok[0] = font name, tok[1] = size
            if (tok[0][0] == '/' && tok[0][1] != '\0') {
                size_t flen = strlen(tok[0] + 1);
                if (flen >= font_sz) flen = font_sz - 1;
                memcpy(font, tok[0] + 1, flen);
                font[flen] = '\0';
            }
            double sz = strtod(tok[1], NULL);
            if (sz > 0 && sz < 1000) *size = sz;
            return;
        }
        memcpy(tok[0], tok[1], sizeof(tok[0]));
        size_t copy = tlen < sizeof(tok[1]) - 1 ? tlen : sizeof(tok[1]) - 1;
        memcpy(tok[1], s + start, copy);
        tok[1][copy] = '\0';
    }
}

// --- appearance stream generation (text fields) ---

// Grow-append for the content buffer (malloc'd, caller frees).
static bool form_buf_append(char **buf, size_t *len, size_t *cap,
                            const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t grown = *cap == 0 ? 256 : *cap;
        while (grown < *len + n + 1) grown *= 2;
        char *nb = (char *)realloc(*buf, grown);
        if (!nb) return false;
        *buf = nb;
        *cap = grown;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

// The font object the appearance stream references: the AcroForm /DR /Font
// entry when it exists (kept as a shared reference), else a synthesized
// WinAnsi Helvetica.
static TspdfObj *form_appearance_font(FormCtx *ctx, const char *da_font) {
    TspdfObj *dr = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(ctx->acroform, "DR"));
    if (dr && dr->type == TSPDF_OBJ_DICT) {
        TspdfObj *fonts = form_resolve(ctx->doc, &ctx->parser,
                                       tspdf_dict_get(dr, "Font"));
        if (fonts && fonts->type == TSPDF_OBJ_DICT) {
            TspdfObj *f = tspdf_dict_get(fonts, da_font);
            if (f && (f->type == TSPDF_OBJ_REF || f->type == TSPDF_OBJ_DICT)) {
                return f;
            }
        }
    }
    TspdfObj *font = form_obj_new(ctx->a, TSPDF_OBJ_DICT);
    if (!font) return NULL;
    if (form_dict_put(font, "Type", form_make_name(ctx->a, "Font"), ctx->a) != TSPDF_OK ||
        form_dict_put(font, "Subtype", form_make_name(ctx->a, "Type1"), ctx->a) != TSPDF_OK ||
        form_dict_put(font, "BaseFont", form_make_name(ctx->a, "Helvetica"), ctx->a) != TSPDF_OK ||
        form_dict_put(font, "Encoding", form_make_name(ctx->a, "WinAnsiEncoding"), ctx->a) != TSPDF_OK) {
        return NULL;
    }
    return font;
}

// Build and attach /AP << /N form-XObject >> showing `value` in the widget's
// rect. Single-line v1: newlines become spaces, no comb/multiline layout;
// the XObject /BBox clips overlong values.
static TspdfError form_set_text_appearance(FormCtx *ctx, TspdfObj *widget,
                                           const char *value) {
    TspdfObj *rect = form_resolve(ctx->doc, &ctx->parser,
                                  tspdf_dict_get(widget, "Rect"));
    if (!rect || rect->type != TSPDF_OBJ_ARRAY || rect->array.count != 4) {
        return TSPDF_OK;  // no rect, nothing to draw (value alone still set)
    }
    double x0 = form_number(form_resolve(ctx->doc, &ctx->parser, &rect->array.items[0]), 0);
    double y0 = form_number(form_resolve(ctx->doc, &ctx->parser, &rect->array.items[1]), 0);
    double x1 = form_number(form_resolve(ctx->doc, &ctx->parser, &rect->array.items[2]), 0);
    double y1 = form_number(form_resolve(ctx->doc, &ctx->parser, &rect->array.items[3]), 0);
    double w = x1 > x0 ? x1 - x0 : x0 - x1;
    double h = y1 > y0 ? y1 - y0 : y0 - y1;
    if (w <= 0 || h <= 0) return TSPDF_OK;

    char da_font[64];
    double size = 0;
    TspdfObj *da = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(widget, "DA"));
    if (!da || da->type != TSPDF_OBJ_STRING) {
        // effective DA was carried on the terminal; the caller passes it via
        // ctx->acroform lookups — widgets usually repeat it, so fall back.
        da = form_resolve(ctx->doc, &ctx->parser,
                          tspdf_dict_get(ctx->acroform, "DA"));
    }
    form_parse_da(da, da_font, sizeof(da_font), &size);
    if (size <= 0) {
        size = h * 0.62;                 // auto-size: fit the rect height
        if (size > 12) size = 12;
        if (size < 4) size = 4;
    }

    // Single line: fold line breaks, then cp1252 (lossy) for a base14 font.
    size_t vlen = strlen(value);
    char *line = (char *)malloc(vlen + 1);
    if (!line) return TSPDF_ERR_ALLOC;
    memcpy(line, value, vlen + 1);
    for (size_t i = 0; i < vlen; i++) {
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    }
    tspdf_utf8_to_cp1252_lossy(line, line, NULL);

    // Content: /Tx BMC q BT <da font+size> 0 g x y Td (line) Tj ET Q EMC
    char *content = NULL;
    size_t clen = 0, ccap = 0;
    char head[256];
    double baseline = (h - size * 0.7) / 2.0;
    if (baseline < 1) baseline = 1;
    int n = snprintf(head, sizeof(head),
                     "/Tx BMC\nq\nBT\n/%s %.2f Tf\n0 g\n2 %.2f Td\n(",
                     da_font, size, baseline);
    bool ok = n > 0 && form_buf_append(&content, &clen, &ccap, head, (size_t)n);
    for (const char *p = line; ok && *p; p++) {
        char c = *p;
        if (c == '(' || c == ')' || c == '\\') {
            char esc[2] = {'\\', c};
            ok = form_buf_append(&content, &clen, &ccap, esc, 2);
        } else {
            ok = form_buf_append(&content, &clen, &ccap, &c, 1);
        }
    }
    if (ok) ok = form_buf_append(&content, &clen, &ccap, ") Tj\nET\nQ\nEMC", 13);
    free(line);
    if (!ok) {
        free(content);
        return TSPDF_ERR_ALLOC;
    }

    // Form XObject: dict + self-contained stream.
    TspdfArena *a = ctx->a;
    TspdfObj *sdict = form_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *bbox = form_obj_new(a, TSPDF_OBJ_ARRAY);
    TspdfObj *resources = form_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *fonts = form_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *font = form_appearance_font(ctx, da_font);
    TspdfObj *stream = form_obj_new(a, TSPDF_OBJ_STREAM);
    if (!sdict || !bbox || !resources || !fonts || !font || !stream) {
        free(content);
        return TSPDF_ERR_ALLOC;
    }
    bbox->array.items = (TspdfObj *)tspdf_arena_alloc_zero(a, 4 * sizeof(TspdfObj));
    if (!bbox->array.items) {
        free(content);
        return TSPDF_ERR_ALLOC;
    }
    bbox->array.count = 4;
    bbox->array.items[0].type = TSPDF_OBJ_REAL;
    bbox->array.items[1].type = TSPDF_OBJ_REAL;
    bbox->array.items[2] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = w};
    bbox->array.items[3] = (TspdfObj){.type = TSPDF_OBJ_REAL, .real = h};

    TspdfError err = form_dict_put(fonts, da_font, font, a);
    if (err == TSPDF_OK) err = form_dict_put(resources, "Font", fonts, a);
    if (err == TSPDF_OK) err = form_dict_put(sdict, "Type", form_make_name(a, "XObject"), a);
    if (err == TSPDF_OK) err = form_dict_put(sdict, "Subtype", form_make_name(a, "Form"), a);
    if (err == TSPDF_OK) err = form_dict_put(sdict, "BBox", bbox, a);
    if (err == TSPDF_OK) err = form_dict_put(sdict, "Resources", resources, a);
    if (err == TSPDF_OK) err = form_dict_put(sdict, "Length", form_make_int(a, (int64_t)clen), a);
    if (err != TSPDF_OK) {
        free(content);
        return err;
    }

    stream->stream.dict = sdict;
    stream->stream.data = (uint8_t *)tspdf_arena_alloc(a, clen);
    if (!stream->stream.data) {
        free(content);
        return TSPDF_ERR_ALLOC;
    }
    memcpy(stream->stream.data, content, clen);
    stream->stream.len = clen;
    stream->stream.self_contained = true;
    free(content);

    uint32_t stream_num = tspdf_register_new_obj(ctx->doc, stream);
    if (stream_num == 0) return TSPDF_ERR_ALLOC;

    TspdfObj *ap = form_obj_new(a, TSPDF_OBJ_DICT);
    TspdfObj *nref = form_make_ref(a, stream_num);
    if (!ap || !nref) return TSPDF_ERR_ALLOC;
    err = form_dict_put(ap, "N", nref, a);
    if (err == TSPDF_OK) err = form_dict_put(widget, "AP", ap, a);
    return err;
}

// --- fill ---

static TspdfError form_fill_button(FormCtx *ctx, FormTerminal *term,
                                   const char *value) {
    // Validate: "Off" or one of the field's on-state names.
    bool is_off = strcmp(value, "Off") == 0;
    if (!is_off) {
        const char **options = NULL;
        size_t option_count = 0;
        TspdfError err = form_button_options(ctx, term, &options, &option_count);
        if (err != TSPDF_OK) return err;
        bool known = form_option_seen(options, option_count, value);
        // Widgets without /AP (e.g. tspdf's own writer) publish no on-state
        // names; accept the caller's state as-is for those.
        if (!known && option_count > 0) return TSPDF_ERR_INVALID_ARG;
    }

    TspdfArena *a = ctx->a;
    TspdfObj *vname = form_make_name(a, value);
    if (!vname) return TSPDF_ERR_ALLOC;
    TspdfError err = form_dict_put(term->field, "V", vname, a);
    if (err != TSPDF_OK) return err;

    for (size_t w = 0; w < term->widget_count && err == TSPDF_OK; w++) {
        TspdfObj *widget = term->widgets[w];
        const char *as = "Off";
        if (!is_off) {
            TspdfObj *ap = form_resolve(ctx->doc, &ctx->parser,
                                        tspdf_dict_get(widget, "AP"));
            TspdfObj *n = ap && ap->type == TSPDF_OBJ_DICT
                ? form_resolve(ctx->doc, &ctx->parser, tspdf_dict_get(ap, "N"))
                : NULL;
            if (n && n->type == TSPDF_OBJ_DICT) {
                as = tspdf_dict_get(n, value) ? value : "Off";
            } else {
                as = value;  // no /AP states to consult
            }
        }
        TspdfObj *asname = form_make_name(a, as);
        if (!asname) return TSPDF_ERR_ALLOC;
        err = form_dict_put(widget, "AS", asname, a);
    }
    return err;
}

static TspdfError form_fill_text(FormCtx *ctx, FormTerminal *term,
                                 const char *value) {
    TspdfObj *vstr = form_make_text_string(ctx->a, value);
    if (!vstr) return TSPDF_ERR_ALLOC;
    TspdfError err = form_dict_put(term->field, "V", vstr, ctx->a);
    for (size_t w = 0; w < term->widget_count && err == TSPDF_OK; w++) {
        err = form_set_text_appearance(ctx, term->widgets[w], value);
    }
    return err;
}

static TspdfError form_fill_choice(FormCtx *ctx, FormTerminal *term,
                                   const char *value) {
    TspdfObj *vstr = form_make_text_string(ctx->a, value);
    if (!vstr) return TSPDF_ERR_ALLOC;
    TspdfError err = form_dict_put(term->field, "V", vstr, ctx->a);
    // A stale selection index would override the new /V in some viewers.
    if (err == TSPDF_OK) form_dict_remove(term->field, "I");
    return err;
}

TspdfError tspdf_reader_form_fill(TspdfReader *doc, const char *name,
                                  const char *value, bool force) {
    if (!doc || !name || !value) return TSPDF_ERR_INVALID_ARG;

    FormCtx ctx;
    TspdfError err = form_collect_terminals(doc, &ctx);
    if (err != TSPDF_OK) return err;

    FormTerminal *term = NULL;
    for (size_t i = 0; i < ctx.count; i++) {
        if (strcmp(ctx.items[i].name, name) == 0) {
            term = &ctx.items[i];
            break;
        }
    }
    if (!term) {
        form_terminals_free(&ctx);
        return TSPDF_ERR_INVALID_ARG;
    }
    if ((term->ff & 1) && !force) {
        form_terminals_free(&ctx);
        return TSPDF_ERR_UNSUPPORTED;
    }

    switch (term->type) {
        case TSPDF_FIELD_TEXT:
            err = form_fill_text(&ctx, term, value);
            break;
        case TSPDF_FIELD_CHECKBOX:
        case TSPDF_FIELD_RADIO:
            err = form_fill_button(&ctx, term, value);
            break;
        case TSPDF_FIELD_CHOICE:
            err = form_fill_choice(&ctx, term, value);
            break;
        default:
            err = TSPDF_ERR_UNSUPPORTED;  // pushbutton/signature/unknown
            break;
    }

    // Belt: ask viewers to regenerate appearances for everything we set.
    if (err == TSPDF_OK && ctx.acroform) {
        TspdfObj *na = form_make_bool(&doc->arena, true);
        if (!na) err = TSPDF_ERR_ALLOC;
        else err = form_dict_put(ctx.acroform, "NeedAppearances", na, &doc->arena);
    }
    if (err == TSPDF_OK) doc->modified = true;
    form_terminals_free(&ctx);
    return err;
}

// --- flatten ---

// Append the value text of a widget to the page content buffer, clipped to
// the widget rect. Uses the shared flattening font (see form_flatten_font).
static void form_flatten_draw_text(FormCtx *ctx, TspdfBuffer *content,
                                   TspdfObj *widget, const char *utf8,
                                   double x0, double y0, double w, double h,
                                   bool *used_font) {
    char da_font[64];
    double size = 0;
    TspdfObj *da = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(widget, "DA"));
    if (!da || da->type != TSPDF_OBJ_STRING) {
        da = form_resolve(ctx->doc, &ctx->parser,
                          tspdf_dict_get(ctx->acroform, "DA"));
    }
    form_parse_da(da, da_font, sizeof(da_font), &size);
    if (size <= 0) {
        size = h * 0.62;
        if (size > 12) size = 12;
        if (size < 4) size = 4;
    }

    size_t vlen = strlen(utf8);
    char *line = (char *)malloc(vlen + 1);
    if (!line) return;
    memcpy(line, utf8, vlen + 1);
    for (size_t i = 0; i < vlen; i++) {
        if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';
    }
    tspdf_utf8_to_cp1252_lossy(line, line, NULL);

    double baseline = y0 + (h - size * 0.7) / 2.0;
    if (baseline < y0 + 1) baseline = y0 + 1;
    tspdf_buffer_printf(content,
                        "q\n%.2f %.2f %.2f %.2f re W n\nBT\n/TspdfFf %.2f Tf\n"
                        "0 g\n%.2f %.2f Td\n(",
                        x0, y0, w, h, size, x0 + 2, baseline);
    for (const char *p = line; *p; p++) {
        if (*p == '(' || *p == ')' || *p == '\\') {
            tspdf_buffer_append_byte(content, '\\');
        }
        tspdf_buffer_append_byte(content, (uint8_t)*p);
    }
    tspdf_buffer_append_str(content, ") Tj\nET\nQ\n");
    *used_font = true;
    free(line);
}

// The on-state of a button widget, or NULL when it is off: /AS wins, then
// the field /V when this widget's /AP /N carries that state.
static const char *form_widget_on_state(FormCtx *ctx, const FormTerminal *term,
                                        TspdfObj *widget) {
    TspdfObj *as = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(widget, "AS"));
    if (as && as->type == TSPDF_OBJ_NAME) {
        if (form_name_is(as, "Off")) return NULL;
        return form_decode_text(ctx->a, as);
    }
    if (term->v && term->v->type == TSPDF_OBJ_NAME &&
        !form_name_is(term->v, "Off")) {
        return form_decode_text(ctx->a, term->v);
    }
    if (term->v && term->v->type == TSPDF_OBJ_STRING && term->v->string.len > 0 &&
        !(term->v->string.len == 3 &&
          memcmp(term->v->string.data, "Off", 3) == 0)) {
        // tspdf's own writer stores checkbox /V as a string.
        return form_decode_text(ctx->a, term->v);
    }
    return NULL;
}

// The /AP /N appearance stream for `state` when it is trivially reusable:
// an indirect reference to a stream with a 4-number /BBox and no /Matrix.
// Returns the object number (for /XObject resources) or 0.
static uint32_t form_widget_ap_stream(FormCtx *ctx, TspdfObj *widget,
                                      const char *state, double bbox[4]) {
    TspdfObj *ap = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(widget, "AP"));
    if (!ap || ap->type != TSPDF_OBJ_DICT) return 0;
    TspdfObj *n_val = tspdf_dict_get(ap, "N");
    TspdfObj *n = form_resolve(ctx->doc, &ctx->parser, n_val);
    TspdfObj *stream_val = NULL;
    if (n && n->type == TSPDF_OBJ_DICT) {
        stream_val = tspdf_dict_get(n, state);
    } else if (n && n->type == TSPDF_OBJ_STREAM) {
        stream_val = n_val;  // direct stream (no per-state dict)
    }
    if (!stream_val || stream_val->type != TSPDF_OBJ_REF) return 0;
    TspdfObj *stream = form_resolve(ctx->doc, &ctx->parser, stream_val);
    if (!stream || stream->type != TSPDF_OBJ_STREAM || !stream->stream.dict) {
        return 0;
    }
    // Only an absent or identity /Matrix is trivially reusable.
    TspdfObj *mtx = form_resolve(ctx->doc, &ctx->parser,
                                 tspdf_dict_get(stream->stream.dict, "Matrix"));
    if (mtx) {
        if (mtx->type != TSPDF_OBJ_ARRAY || mtx->array.count != 6) return 0;
        static const double identity[6] = {1, 0, 0, 1, 0, 0};
        for (size_t i = 0; i < 6; i++) {
            double m = form_number(form_resolve(ctx->doc, &ctx->parser,
                                                &mtx->array.items[i]), 0);
            if (m != identity[i]) return 0;
        }
    }
    TspdfObj *bb = form_resolve(ctx->doc, &ctx->parser,
                                tspdf_dict_get(stream->stream.dict, "BBox"));
    if (!bb || bb->type != TSPDF_OBJ_ARRAY || bb->array.count != 4) return 0;
    for (size_t i = 0; i < 4; i++) {
        bbox[i] = form_number(form_resolve(ctx->doc, &ctx->parser,
                                           &bb->array.items[i]), 0);
    }
    if (!(bbox[2] - bbox[0] > 0) || !(bbox[3] - bbox[1] > 0)) return 0;
    return stream_val->ref.num;
}

// Vector check mark inside the rect (fallback when no /AP stream applies).
static void form_flatten_draw_check(TspdfBuffer *content, double x0, double y0,
                                    double w, double h) {
    tspdf_buffer_printf(content,
                        "q\n0 g 1.5 w\n%.2f %.2f m\n%.2f %.2f l\n%.2f %.2f l\nS\nQ\n",
                        x0 + 0.20 * w, y0 + 0.55 * h,
                        x0 + 0.42 * w, y0 + 0.25 * h,
                        x0 + 0.80 * w, y0 + 0.75 * h);
}

TspdfError tspdf_reader_form_flatten(TspdfReader *doc) {
    if (!doc) return TSPDF_ERR_INVALID_ARG;

    FormCtx ctx;
    TspdfError err = form_collect_terminals(doc, &ctx);
    if (err != TSPDF_OK) return err;

    TspdfObj *catalog = form_catalog(doc, &ctx.parser);
    if (!ctx.acroform || !catalog) {
        form_terminals_free(&ctx);
        return TSPDF_OK;
    }

    size_t map_size = 0;
    size_t *annot_map = form_build_annot_page_map(doc, &ctx.parser, &map_size);
    TspdfArena *a = &doc->arena;
    uint32_t font_num = 0;  // shared flattening font, registered on first use

    // Stamp values page by page.
    for (size_t p = 0; p < doc->pages.count && err == TSPDF_OK; p++) {
        TspdfBuffer content = tspdf_buffer_create(256);
        TspdfObj *xobjects = NULL;  // /XObject additions for this page
        bool used_font = false;
        size_t xobj_counter = 0;

        for (size_t i = 0; i < ctx.count && err == TSPDF_OK; i++) {
            FormTerminal *term = &ctx.items[i];
            for (size_t wi = 0; wi < term->widget_count && err == TSPDF_OK; wi++) {
                TspdfObj *widget = term->widgets[wi];
                if (form_widget_page(doc, &ctx.parser, widget,
                                     term->widget_nums[wi], annot_map,
                                     map_size) != p) {
                    continue;
                }
                TspdfObj *rect = form_resolve(doc, &ctx.parser,
                                              tspdf_dict_get(widget, "Rect"));
                if (!rect || rect->type != TSPDF_OBJ_ARRAY ||
                    rect->array.count != 4) {
                    continue;
                }
                double rx0 = form_number(form_resolve(doc, &ctx.parser, &rect->array.items[0]), 0);
                double ry0 = form_number(form_resolve(doc, &ctx.parser, &rect->array.items[1]), 0);
                double rx1 = form_number(form_resolve(doc, &ctx.parser, &rect->array.items[2]), 0);
                double ry1 = form_number(form_resolve(doc, &ctx.parser, &rect->array.items[3]), 0);
                double x0 = rx0 < rx1 ? rx0 : rx1;
                double y0 = ry0 < ry1 ? ry0 : ry1;
                double w = rx0 < rx1 ? rx1 - rx0 : rx0 - rx1;
                double h = ry0 < ry1 ? ry1 - ry0 : ry0 - ry1;
                if (w <= 0 || h <= 0) continue;

                if (term->type == TSPDF_FIELD_TEXT ||
                    term->type == TSPDF_FIELD_CHOICE) {
                    if (!term->v || (term->v->type != TSPDF_OBJ_STRING &&
                                     term->v->type != TSPDF_OBJ_NAME)) {
                        continue;
                    }
                    const char *utf8 = form_decode_text(a, term->v);
                    if (!utf8) {
                        err = TSPDF_ERR_ALLOC;
                        break;
                    }
                    if (utf8[0] == '\0') continue;
                    form_flatten_draw_text(&ctx, &content, widget, utf8,
                                           x0, y0, w, h, &used_font);
                } else if (term->type == TSPDF_FIELD_CHECKBOX ||
                           term->type == TSPDF_FIELD_RADIO) {
                    const char *state = form_widget_on_state(&ctx, term, widget);
                    if (!state) continue;
                    double bbox[4] = {0};
                    uint32_t ap_num = form_widget_ap_stream(&ctx, widget,
                                                            state, bbox);
                    if (ap_num > 0) {
                        char xname[32];
                        snprintf(xname, sizeof(xname), "TspdfFx%zu",
                                 xobj_counter++);
                        if (!xobjects) xobjects = form_obj_new(a, TSPDF_OBJ_DICT);
                        TspdfObj *ref = form_make_ref(a, ap_num);
                        if (!xobjects || !ref) {
                            err = TSPDF_ERR_ALLOC;
                            break;
                        }
                        err = form_dict_put(xobjects, xname, ref, a);
                        if (err != TSPDF_OK) break;
                        double bw = bbox[2] - bbox[0];
                        double bh = bbox[3] - bbox[1];
                        double sx = w / bw;
                        double sy = h / bh;
                        tspdf_buffer_printf(&content,
                                            "q\n%.4f 0 0 %.4f %.2f %.2f cm\n/%s Do\nQ\n",
                                            sx, sy, x0 - bbox[0] * sx,
                                            y0 - bbox[1] * sy, xname);
                    } else {
                        form_flatten_draw_check(&content, x0, y0, w, h);
                    }
                }
            }
        }

        if (err == TSPDF_OK && content.len > 0) {
            // Build the resource additions and merge them into the page,
            // rewriting our operator names on collision.
            TspdfObj *new_res = form_obj_new(a, TSPDF_OBJ_DICT);
            if (!new_res) err = TSPDF_ERR_ALLOC;
            if (err == TSPDF_OK && used_font) {
                if (font_num == 0) {
                    TspdfObj *font = form_appearance_font(&ctx, "TspdfFf");
                    if (font) font_num = tspdf_register_new_obj(doc, font);
                    if (font_num == 0) err = TSPDF_ERR_ALLOC;
                }
                if (err == TSPDF_OK) {
                    TspdfObj *fonts = form_obj_new(a, TSPDF_OBJ_DICT);
                    TspdfObj *fref = form_make_ref(a, font_num);
                    if (!fonts || !fref) err = TSPDF_ERR_ALLOC;
                    if (err == TSPDF_OK) err = form_dict_put(fonts, "TspdfFf", fref, a);
                    if (err == TSPDF_OK) err = form_dict_put(new_res, "Font", fonts, a);
                }
            }
            if (err == TSPDF_OK && xobjects) {
                err = form_dict_put(new_res, "XObject", xobjects, a);
            }

            TspdfRenameMap renames = {0};
            if (err == TSPDF_OK && new_res->dict.count > 0) {
                // tspdf_resources_merge treats a non-dict /Resources value as
                // absent and would append a duplicate key: pages often carry
                // /Resources as an indirect ref, so inline the resolved dict
                // first (drop the entry when the ref dangles).
                TspdfObj *page_dict = doc->pages.pages[p].page_dict;
                TspdfObj *res_val = tspdf_dict_get(page_dict, "Resources");
                if (res_val && res_val->type == TSPDF_OBJ_REF) {
                    TspdfObj *res = form_resolve(doc, &ctx.parser, res_val);
                    if (res && res->type == TSPDF_OBJ_DICT) {
                        err = form_dict_put(page_dict, "Resources", res, a);
                    } else {
                        form_dict_remove(page_dict, "Resources");
                    }
                }
                if (err == TSPDF_OK) {
                    err = tspdf_resources_merge(page_dict, new_res, a, &renames);
                }
            }
            const uint8_t *final_content = content.data;
            size_t final_len = content.len;
            if (err == TSPDF_OK && renames.count > 0) {
                size_t rewritten_len = 0;
                uint8_t *rewritten = tspdf_content_rewrite(content.data,
                    content.len, &renames, &rewritten_len, a);
                if (rewritten) {
                    final_content = rewritten;
                    final_len = rewritten_len;
                }
            }
            if (err == TSPDF_OK) {
                TspdfStream *stream = tspdf_page_begin_content(doc, p);
                if (!stream) {
                    err = TSPDF_ERR_ALLOC;
                } else {
                    tspdf_buffer_append(&stream->buf, final_content, final_len);
                    err = tspdf_page_end_content(doc, p, stream, NULL);
                }
            }
        }
        tspdf_buffer_destroy(&content);
    }

    // Remove widget annotations from every page, then drop the form itself.
    for (size_t p = 0; p < doc->pages.count && err == TSPDF_OK; p++) {
        TspdfObj *page_dict = doc->pages.pages[p].page_dict;
        TspdfObj *annots = form_resolve(doc, &ctx.parser,
                                        tspdf_dict_get(page_dict, "Annots"));
        if (!annots || annots->type != TSPDF_OBJ_ARRAY || annots->array.count == 0) {
            continue;
        }
        TspdfObj *kept = form_obj_new(a, TSPDF_OBJ_ARRAY);
        if (!kept) {
            err = TSPDF_ERR_ALLOC;
            break;
        }
        kept->array.items = (TspdfObj *)tspdf_arena_alloc_zero(a,
            annots->array.count * sizeof(TspdfObj));
        if (!kept->array.items) {
            err = TSPDF_ERR_ALLOC;
            break;
        }
        for (size_t i = 0; i < annots->array.count; i++) {
            TspdfObj *annot = form_resolve(doc, &ctx.parser,
                                           &annots->array.items[i]);
            if (annot && annot->type == TSPDF_OBJ_DICT &&
                form_name_is(form_resolve(doc, &ctx.parser,
                                          tspdf_dict_get(annot, "Subtype")),
                             "Widget")) {
                continue;
            }
            kept->array.items[kept->array.count++] = annots->array.items[i];
        }
        if (kept->array.count == annots->array.count) continue;
        if (kept->array.count == 0) {
            form_dict_remove(page_dict, "Annots");
        } else {
            err = form_dict_put(page_dict, "Annots", kept, a);
        }
    }

    if (err == TSPDF_OK) {
        form_dict_remove(catalog, "AcroForm");
        doc->modified = true;
    }
    free(annot_map);
    form_terminals_free(&ctx);
    return err;
}
