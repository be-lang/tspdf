// AcroForm form fields: enumerate, fill, flatten (feat/form).
//
// All of this walks attacker-controlled structure (the field tree hangs off
// the catalog), so every recursion carries the same guards as tspr_doctree.c:
// a depth cap plus a shared node budget, because cyclic /Kids fan out to
// count^depth visits under a depth cap alone.

#include "tspr_internal.h"
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

TspdfError tspdf_reader_form_fill(TspdfReader *doc, const char *name,
                                  const char *value, bool force) {
    (void)doc; (void)name; (void)value; (void)force;
    return TSPDF_ERR_UNSUPPORTED;
}

TspdfError tspdf_reader_form_flatten(TspdfReader *doc) {
    (void)doc;
    return TSPDF_ERR_UNSUPPORTED;
}
