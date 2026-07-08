#include "tspr_internal.h"
#include <string.h>
#include <stdio.h>

// --- Helper: add a rename entry to the map ---

static void rename_map_add(TspdfRenameMap *map, TspdfArena *arena,
                           const char *old_name, const char *new_name) {
    if (map->count >= map->capacity) {
        size_t new_cap = map->capacity == 0 ? 8 : map->capacity * 2;
        TspdfResourceRename *new_arr = tspdf_arena_alloc(arena, new_cap * sizeof(TspdfResourceRename));
        if (!new_arr) return;
        if (map->renames && map->count > 0) {
            memcpy(new_arr, map->renames, map->count * sizeof(TspdfResourceRename));
        }
        map->renames = new_arr;
        map->capacity = new_cap;
    }
    size_t idx = map->count++;
    size_t olen = strlen(old_name);
    map->renames[idx].old_name = tspdf_arena_alloc(arena, olen + 1);
    memcpy(map->renames[idx].old_name, old_name, olen + 1);
    size_t nlen = strlen(new_name);
    map->renames[idx].new_name = tspdf_arena_alloc(arena, nlen + 1);
    memcpy(map->renames[idx].new_name, new_name, nlen + 1);
}

// --- Helper: check if a key exists in a dict ---

static bool dict_has_key(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return false;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0)
            return true;
    }
    return false;
}

// --- Helper: add an entry to a dict, rebuilding entries array in arena ---

static void dict_add_entry(TspdfObj *dict, TspdfArena *arena,
                           const char *key, TspdfObj *value) {
    size_t old_count = dict->dict.count;
    size_t new_count = old_count + 1;
    TspdfDictEntry *new_entries = tspdf_arena_alloc(arena, new_count * sizeof(TspdfDictEntry));
    if (!new_entries) return;
    if (dict->dict.entries && old_count > 0) {
        memcpy(new_entries, dict->dict.entries, old_count * sizeof(TspdfDictEntry));
    }
    size_t klen = strlen(key);
    new_entries[old_count].key = tspdf_arena_alloc(arena, klen + 1);
    memcpy(new_entries[old_count].key, key, klen + 1);
    new_entries[old_count].value = value;
    dict->dict.entries = new_entries;
    dict->dict.count = new_count;
}

// --- Helper: get or create a sub-dict in a dict ---

static TspdfObj *get_or_create_subdict(TspdfObj *dict, TspdfArena *arena, const char *key) {
    TspdfObj *sub = tspdf_dict_get(dict, key);
    if (sub && sub->type == TSPDF_OBJ_DICT) return sub;
    // Create new empty sub-dict
    sub = tspdf_arena_alloc_zero(arena, sizeof(TspdfObj));
    if (!sub) return NULL;
    sub->type = TSPDF_OBJ_DICT;
    sub->dict.entries = NULL;
    sub->dict.count = 0;
    dict_add_entry(dict, arena, key, sub);
    return sub;
}

// --- Helper: generate a unique name by appending _2, _3, etc. ---

static char *generate_unique_name(TspdfObj *existing_dict, TspdfArena *arena, const char *base) {
    char buf[256];
    for (int suffix = 2; suffix < 1000; suffix++) {
        snprintf(buf, sizeof(buf), "%s_%d", base, suffix);
        if (!dict_has_key(existing_dict, buf)) {
            size_t len = strlen(buf);
            char *result = tspdf_arena_alloc(arena, len + 1);
            if (result) memcpy(result, buf, len + 1);
            return result;
        }
    }
    return NULL; // extremely unlikely
}

// --- Merge one resource category ---

static void merge_category(TspdfObj *existing_dict, TspdfObj *new_dict,
                           TspdfArena *arena, TspdfRenameMap *renames) {
    if (!new_dict || new_dict->type != TSPDF_OBJ_DICT) return;

    for (size_t i = 0; i < new_dict->dict.count; i++) {
        const char *name = new_dict->dict.entries[i].key;
        TspdfObj *value = new_dict->dict.entries[i].value;

        if (!dict_has_key(existing_dict, name)) {
            // Name not in existing -- add directly
            dict_add_entry(existing_dict, arena, name, value);
        } else {
            // Name collision -- generate unique name and record rename
            char *new_name = generate_unique_name(existing_dict, arena, name);
            if (new_name) {
                dict_add_entry(existing_dict, arena, new_name, value);
                rename_map_add(renames, arena, name, new_name);
            }
        }
    }
}

// --- Public: merge new resources into page's resource dict ---

TspdfError tspdf_resources_merge(TspdfObj *page_dict, TspdfObj *new_resources,
                                TspdfArena *arena, TspdfRenameMap *renames) {
    if (!page_dict || page_dict->type != TSPDF_OBJ_DICT) return TSPDF_ERR_INVALID_PDF;
    if (!new_resources || new_resources->type != TSPDF_OBJ_DICT) return TSPDF_OK;

    // Get or create /Resources on the page
    TspdfObj *resources = get_or_create_subdict(page_dict, arena, "Resources");
    if (!resources) return TSPDF_ERR_ALLOC;

    // Merge each category
    static const char *categories[] = { "Font", "XObject", "ExtGState" };
    for (int c = 0; c < 3; c++) {
        TspdfObj *new_sub = tspdf_dict_get(new_resources, categories[c]);
        if (!new_sub || new_sub->type != TSPDF_OBJ_DICT) continue;

        TspdfObj *existing_sub = get_or_create_subdict(resources, arena, categories[c]);
        if (!existing_sub) return TSPDF_ERR_ALLOC;

        merge_category(existing_sub, new_sub, arena, renames);
    }

    return TSPDF_OK;
}

// --- Public: rewrite content stream, replacing renamed resource names ---

uint8_t *tspdf_content_rewrite(const uint8_t *stream, size_t len,
                               const TspdfRenameMap *renames, size_t *out_len,
                               TspdfArena *arena) {
    if (!renames || renames->count == 0) return NULL;

    // Worst case: each rename could make the name longer. Estimate extra space.
    // Each rename adds at most ~10 chars per occurrence. Generous buffer.
    size_t max_extra = renames->count * 64;
    size_t buf_size = len + max_extra;
    uint8_t *out = tspdf_arena_alloc(arena, buf_size);
    if (!out) return NULL;

    size_t opos = 0;
    size_t i = 0;

    while (i < len) {
        if (stream[i] == '/') {
            // Found a name token - read it
            size_t name_start = i + 1; // skip the '/'
            size_t name_end = name_start;
            while (name_end < len) {
                uint8_t ch = stream[name_end];
                if (ch <= ' ' || ch == '(' || ch == ')' || ch == '<' ||
                    ch == '>' || ch == '[' || ch == ']' || ch == '{' ||
                    ch == '}' || ch == '/' || ch == '%') {
                    break;
                }
                name_end++;
            }
            size_t name_len = name_end - name_start;

            // Check against renames
            bool renamed = false;
            for (size_t r = 0; r < renames->count; r++) {
                size_t old_len = strlen(renames->renames[r].old_name);
                if (name_len == old_len &&
                    memcmp(stream + name_start, renames->renames[r].old_name, old_len) == 0) {
                    // Write /new_name
                    const char *new_name = renames->renames[r].new_name;
                    size_t new_len = strlen(new_name);
                    out[opos++] = '/';
                    memcpy(out + opos, new_name, new_len);
                    opos += new_len;
                    renamed = true;
                    break;
                }
            }

            if (!renamed) {
                // Copy as-is: / + name
                memcpy(out + opos, stream + i, 1 + name_len);
                opos += 1 + name_len;
            }
            i = name_end;
        } else {
            out[opos++] = stream[i++];
        }
    }

    *out_len = opos;
    return out;
}
