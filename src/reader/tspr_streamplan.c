// Two save-pipeline decisions, factored out of tspr_serialize.c so the writers
// are thin executors (the info_plan mold — see tspr_infoplan.c):
//
//   1. tspdf_stream_plan: given a stream dict and the recompress option, decide
//      which recompression strategy applies (keep / re-deflate / add-flate /
//      armor-strip). Pure dict inspection, no bytes read and no deflate run, so
//      the six intertwined branches that used to live in the writer are now
//      matrix-testable in isolation. The executor still runs deflate and keeps
//      whichever encoding is smaller (the never-grow rule).
//
//   2. tspdf_collect_objects: the object-collection walk shared by the plain,
//      encrypted, and preserve-ids paths. It filters a visited[] mark array
//      into the ordered emit list and builds the renumber map; the three paths
//      keep their own emission but stop duplicating the collection filter.

#include "tspr_internal.h"
#include <stdlib.h>
#include <string.h>

// --- Stream recompression plan ---

// Image XObjects get the fast deflate level during recompression: their Flate
// payloads (PNG-style, usually predictor-coded) gain almost nothing from the
// slow best-effort search, and scan-heavy files would otherwise dominate
// `tspdf compress` wall time for no size benefit.
static bool stream_is_image(TspdfObj *dict) {
    TspdfObj *st = tspdf_dict_get(dict, "Subtype");
    return st && st->type == TSPDF_OBJ_NAME &&
           strcmp((const char *)st->string.data, "Image") == 0;
}

// True when the /Filter chain consists only of lossless byte filters our
// decoder can round-trip AND rewriting it as a bare /FlateDecode can win:
// either it contains a non-Flate "armor" stage (ASCIIHex/ASCII85/RunLength/
// LZW — those cost 25%+, hex doubles) or it is an array form like
// [/FlateDecode] (common in generator output; re-encoding normalizes it and
// applies the best-effort level). A bare FlateDecode NAME (with or without
// predictor params) is handled by the cheaper re-flate branch instead — the
// predictor usually helps compression, so full-decode-and-replace would
// only burn time to then keep the original.
static bool filter_chain_is_strippable(TspdfObj *filter) {
    if (!filter) return false;
    size_t count = filter->type == TSPDF_OBJ_ARRAY ? filter->array.count : 1;
    if (filter->type != TSPDF_OBJ_NAME && filter->type != TSPDF_OBJ_ARRAY) return false;
    bool has_armor = false;
    for (size_t i = 0; i < count; i++) {
        TspdfObj *name = filter->type == TSPDF_OBJ_NAME ? filter : &filter->array.items[i];
        if (!name || name->type != TSPDF_OBJ_NAME) return false;
        const char *f = (const char *)name->string.data;
        if (strcmp(f, "FlateDecode") == 0 || strcmp(f, "Fl") == 0) {
            continue;
        }
        if (strcmp(f, "ASCIIHexDecode") == 0 || strcmp(f, "AHx") == 0 ||
            strcmp(f, "ASCII85Decode") == 0 || strcmp(f, "A85") == 0 ||
            strcmp(f, "RunLengthDecode") == 0 || strcmp(f, "RL") == 0 ||
            strcmp(f, "LZWDecode") == 0 || strcmp(f, "LZW") == 0) {
            has_armor = true;
            continue;
        }
        return false;  // lossy or unknown filter: leave the stream verbatim
    }
    return has_armor || filter->type == TSPDF_OBJ_ARRAY;
}

// True when the stream's /DecodeParms (or /DP) is absent or fully direct —
// null, a dict, or an array of dicts/nulls. tspdf_stream_decode looks the
// parameters up per filter but does NOT resolve indirect references: a
// "/DecodeParms 6 0 R" (or a ref inside the array form) silently yields no
// parameters, so a predictor or LZW /EarlyChange 0 would be skipped and the
// "decoded" bytes would be wrong. The armor-strip path bakes those bytes
// into the output as plain Flate, so it must refuse any parms it cannot see.
// This is the indirect-/DecodeParms armor-strip refusal — a past shipped-bug
// fix, pinned by the test matrix.
static bool decode_parms_fully_direct(TspdfObj *dict) {
    TspdfObj *parms = tspdf_dict_get(dict, "DecodeParms");
    if (!parms) {
        parms = tspdf_dict_get(dict, "DP");
    }
    if (!parms || parms->type == TSPDF_OBJ_NULL || parms->type == TSPDF_OBJ_DICT) {
        return true;
    }
    if (parms->type == TSPDF_OBJ_ARRAY) {
        for (size_t i = 0; i < parms->array.count; i++) {
            TspdfObj *entry = &parms->array.items[i];
            if (entry->type != TSPDF_OBJ_DICT && entry->type != TSPDF_OBJ_NULL) {
                return false;
            }
        }
        return true;
    }
    return false;  // indirect reference or unexpected type
}

TspdfStreamPlan tspdf_stream_plan(TspdfObj *stream_obj, bool recompress,
                                  bool is_xref, size_t stream_len) {
    TspdfStreamPlan plan = { TSPDF_STREAM_KEEP, false };

    if (!recompress || is_xref || !stream_obj ||
        stream_obj->type != TSPDF_OBJ_STREAM) {
        return plan;
    }
    TspdfObj *dict = stream_obj->stream.dict;
    if (!dict) return plan;

    plan.use_fast_level = stream_is_image(dict);
    TspdfObj *filter = tspdf_dict_get(dict, "Filter");

    if (filter && filter->type == TSPDF_OBJ_NAME &&
        strcmp((char *)filter->string.data, "FlateDecode") == 0) {
        plan.action = TSPDF_STREAM_REDEFLATE;
    } else if (!filter && stream_len > 0 &&
               !tspdf_dict_get(dict, "FFilter") &&
               !tspdf_dict_get(dict, "DecodeParms") &&
               !tspdf_dict_get(dict, "DP") &&
               !tspr_is_xmp_metadata(stream_obj)) {
        // Unfiltered stream: deflate + add /Filter. XMP metadata streams stay
        // uncompressed (PDF/A-style scanning), as does anything with
        // external-file or decode-parameter entries.
        plan.action = TSPDF_STREAM_ADD_FLATE;
    } else if (filter_chain_is_strippable(filter) &&
               !tspdf_dict_get(dict, "FFilter") &&
               decode_parms_fully_direct(dict)) {
        // Lossy/unknown filters never reach here (filter_chain_is_strippable),
        // and an indirect /DecodeParms the decoder cannot resolve refuses the
        // strip (decode_parms_fully_direct).
        plan.action = TSPDF_STREAM_ARMOR_STRIP;
    }
    // else: KEEP (already the default)

    return plan;
}

// --- Save-object collection ---

// Resolve a source object by number for collection filtering (mirrors the
// static resolve_for_collect in tspr_serialize.c). Kept here so the shared
// walk can inspect object types without threading a callback.
static TspdfObj *collect_resolve(TspdfReader *doc, uint32_t num,
                                 TspdfParser *parser) {
    if (num < doc->xref.count) {
        return tspdf_xref_resolve(&doc->xref, parser, num, doc->obj_cache,
                                  doc->crypt);
    }
    size_t idx = num - doc->xref.count;
    if (idx < doc->new_objs.count) return doc->new_objs.objs[idx];
    return NULL;
}

// True when object `i` should be dropped from the collected set. Centralizes
// the filter the plain and encrypted standard paths applied inline.
static bool collect_should_skip(TspdfReader *doc, TspdfParser *parser,
                                uint32_t i, bool skip_root,
                                uint32_t source_root_num, bool strip_metadata,
                                uint32_t source_info_num) {
    // The synthetic-catalog paths emit the root as object 1 themselves.
    if (skip_root && source_root_num > 0 && i == source_root_num) {
        return true;
    }
    // Source ObjStm/XRef containers are re-derived (members written or
    // re-packed individually); copying them would duplicate every packed
    // object.
    if (tspr_is_xref_machinery(collect_resolve(doc, i, parser))) {
        return true;
    }
    if (strip_metadata) {
        if (source_info_num > 0 && i == source_info_num) {
            return true;
        }
        if (tspr_is_xmp_metadata(collect_resolve(doc, i, parser))) {
            return true;
        }
    }
    return false;
}

void tspdf_collect_objects(TspdfReader *doc, TspdfParser *parser,
                           const bool *visited, size_t total_objs,
                           uint32_t base_num, bool skip_root,
                           uint32_t source_root_num, bool strip_metadata,
                           uint32_t source_info_num,
                           TspdfCollectResult *res) {
    res->collected = NULL;
    res->collected_count = 0;
    res->map.old_to_new = NULL;
    res->map.map_size = 0;
    res->ok = false;

    uint32_t *map = (uint32_t *)calloc(total_objs > 0 ? total_objs : 1,
                                       sizeof(uint32_t));
    if (!map) return;

    // First pass: count survivors.
    size_t count = 0;
    for (size_t i = 1; i < total_objs; i++) {
        if (!visited[i]) continue;
        if (collect_should_skip(doc, parser, (uint32_t)i, skip_root,
                                source_root_num, strip_metadata,
                                source_info_num)) {
            continue;
        }
        count++;
    }

    uint32_t *collected =
        (uint32_t *)malloc(sizeof(uint32_t) * (count > 0 ? count : 1));
    if (!collected) {
        free(map);
        return;
    }

    // Second pass: record survivors and assign new numbers in order.
    size_t ci = 0;
    uint32_t next_num = base_num;
    for (size_t i = 1; i < total_objs; i++) {
        if (!visited[i]) continue;
        if (collect_should_skip(doc, parser, (uint32_t)i, skip_root,
                                source_root_num, strip_metadata,
                                source_info_num)) {
            continue;
        }
        collected[ci++] = (uint32_t)i;
        map[i] = next_num++;
    }

    res->collected = collected;
    res->collected_count = count;
    res->map.old_to_new = map;
    res->map.map_size = total_objs;
    res->ok = true;
}
