#include "font_subset.h"
#include "../util/buffer.h"
#include <stdlib.h>
#include <string.h>

// --- Endian helpers ---

static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]); }
static int16_t  ri16(const uint8_t *p) { return (int16_t)r16(p); }

static void w16(uint8_t *p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

// --- Pad buffer to 4-byte boundary ---

static void buffer_pad4(TspdfBuffer *b) {
    while (b->len & 3) tspdf_buffer_append_byte(b, 0);
}

// --- Get glyph offset and length from loca table ---

static bool get_glyph_loc(const TTF_Font *font, uint16_t glyph_id,
                           uint32_t *offset, uint32_t *length) {
    const uint8_t *data = font->data;
    uint32_t loca_off = font->loca_offset;

    if (glyph_id >= font->num_glyphs) return false;

    uint32_t start, end;
    if (font->index_to_loc_format == 0) {
        // Short format: offsets are uint16, multiply by 2
        if (loca_off + (glyph_id + 1) * 2 + 2 > font->data_len) return false;
        start = (uint32_t)r16(data + loca_off + glyph_id * 2) * 2;
        end   = (uint32_t)r16(data + loca_off + (glyph_id + 1) * 2) * 2;
    } else {
        // Long format: offsets are uint32
        if (loca_off + (glyph_id + 1) * 4 + 4 > font->data_len) return false;
        start = r32(data + loca_off + glyph_id * 4);
        end   = r32(data + loca_off + (glyph_id + 1) * 4);
    }

    *offset = start;
    *length = (end > start) ? (end - start) : 0;
    return true;
}

// --- Scan composite glyph for component glyph IDs ---

#define COMPOSITE_FLAG_ARG_1_AND_2_ARE_WORDS  0x0001
#define COMPOSITE_FLAG_MORE_COMPONENTS        0x0020

static void scan_composite_components_r(const TTF_Font *font, uint16_t glyph_id,
                                         bool *used_out, int depth) {
    if (depth > 16) return;  // guard against circular/deep composite glyphs
    uint32_t goff, glen;
    if (!get_glyph_loc(font, glyph_id, &goff, &glen)) return;
    if (glen == 0) return;

    const uint8_t *glyf = font->data + font->glyf_offset;
    if (goff + glen > font->glyf_length) return;

    const uint8_t *p = glyf + goff;
    int16_t num_contours = ri16(p);
    if (num_contours >= 0) return;  // simple glyph, not composite

    // Skip header: numberOfContours(2) + xMin(2) + yMin(2) + xMax(2) + yMax(2) = 10
    uint32_t pos = 10;

    while (pos + 4 <= glen) {
        uint16_t flags = r16(p + pos);
        uint16_t component_id = r16(p + pos + 2);
        pos += 4;

        if (component_id < font->num_glyphs && !used_out[component_id]) {
            used_out[component_id] = true;
            // Recursively scan if this component is also composite
            scan_composite_components_r(font, component_id, used_out, depth + 1);
        }

        // Skip transform data based on flags
        if (flags & COMPOSITE_FLAG_ARG_1_AND_2_ARE_WORDS)
            pos += 4;  // two int16 args
        else
            pos += 2;  // two int8 args

        if (flags & 0x0008)      pos += 2;  // WE_HAVE_A_SCALE: one F2Dot14
        else if (flags & 0x0040) pos += 4;  // WE_HAVE_AN_X_AND_Y_SCALE: two F2Dot14
        else if (flags & 0x0080) pos += 8;  // WE_HAVE_A_TWO_BY_TWO: four F2Dot14

        if (!(flags & COMPOSITE_FLAG_MORE_COMPONENTS)) break;
    }
}

// --- Calculate table checksum ---

static uint32_t table_checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    size_t nwords = (len + 3) / 4;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t val = 0;
        for (int b = 0; b < 4; b++) {
            size_t idx = i * 4 + b;
            val = (val << 8) | (idx < len ? data[idx] : 0);
        }
        sum += val;
    }
    return sum;
}

// --- Table output entry (used during subset assembly) ---

typedef struct {
    uint32_t tag;
    const uint8_t *data;
    size_t len;
    bool owned;  // if true, we allocated the data
} TableOut;

static int table_out_cmp(const void *a, const void *b) {
    uint32_t ta = ((const TableOut *)a)->tag;
    uint32_t tb = ((const TableOut *)b)->tag;
    return (ta > tb) - (ta < tb);
}

// --- Build subset TTF ---

uint8_t *ttf_subset(const TTF_Font *font, const bool *used_glyphs, size_t *out_len) {
    if (!font->loca_offset || !font->glyf_offset) {
        return NULL;
    }

    uint16_t ng = font->num_glyphs;

    // Build resolved used set (including composite components)
    bool *used = (bool *)calloc(ng, sizeof(bool));
    if (!used) return NULL;

    used[0] = true;  // always include .notdef
    for (uint16_t i = 0; i < ng; i++) {
        if (used_glyphs[i]) {
            used[i] = true;
            scan_composite_components_r(font, i, used, 0);
        }
    }

    // --- Build new glyf and loca tables ---
    // We keep glyph IDs stable: same count, but unused glyphs become empty (0 bytes)
    TspdfBuffer new_glyf = tspdf_buffer_create(font->glyf_length / 2);  // estimate: smaller

    // loca: need ng+1 entries
    bool use_long_loca = false;
    uint32_t *offsets = (uint32_t *)calloc(ng + 1, sizeof(uint32_t));
    if (!offsets) { free(used); tspdf_buffer_destroy(&new_glyf); return NULL; }

    for (uint16_t i = 0; i < ng; i++) {
        offsets[i] = (uint32_t)new_glyf.len;
        if (used[i]) {
            uint32_t goff, glen;
            if (get_glyph_loc(font, i, &goff, &glen) && glen > 0) {
                const uint8_t *src = font->data + font->glyf_offset + goff;
                if (goff + glen <= font->glyf_length) {
                    tspdf_buffer_append(&new_glyf, src, glen);
                    // Pad each glyph to 2-byte boundary (required for short loca)
                    if (new_glyf.len & 1) {
                        tspdf_buffer_append_byte(&new_glyf, 0);
                    }
                }
            }
        }
    }
    offsets[ng] = (uint32_t)new_glyf.len;
    buffer_pad4(&new_glyf);

    // Determine loca format
    for (uint16_t i = 0; i <= ng; i++) {
        if (offsets[i] > 0x1FFFE) { use_long_loca = true; break; }
    }

    // Build loca table
    TspdfBuffer new_loca;
    if (use_long_loca) {
        new_loca = tspdf_buffer_create((ng + 1) * 4);
        for (uint16_t i = 0; i <= ng; i++) {
            uint8_t buf[4];
            w32(buf, offsets[i]);
            tspdf_buffer_append(&new_loca, buf, 4);
        }
    } else {
        new_loca = tspdf_buffer_create((ng + 1) * 2);
        for (uint16_t i = 0; i <= ng; i++) {
            uint8_t buf[2];
            w16(buf, (uint16_t)(offsets[i] / 2));
            tspdf_buffer_append(&new_loca, buf, 2);
        }
    }
    buffer_pad4(&new_loca);

    free(offsets);
    free(used);

    // --- Collect tables to write ---
    // We include all original tables except glyf and loca (replaced with subset versions)
    // Also patch head table's indexToLocFormat if changed

    int max_tables = font->num_tables + 2;
    TableOut *tables = (TableOut *)calloc(max_tables, sizeof(TableOut));
    int table_count = 0;

    uint32_t tag_glyf = make_tag("glyf");
    uint32_t tag_loca = make_tag("loca");
    uint32_t tag_head = make_tag("head");

    for (int i = 0; i < font->num_tables; i++) {
        uint32_t tag = font->tables[i].tag;
        TableOut *t = &tables[table_count++];
        t->tag = tag;

        if (tag == tag_glyf) {
            t->data = new_glyf.data;
            t->len = new_glyf.len;
            t->owned = false;  // freed separately
        } else if (tag == tag_loca) {
            t->data = new_loca.data;
            t->len = new_loca.len;
            t->owned = false;
        } else if (tag == tag_head) {
            // Copy and potentially patch indexToLocFormat
            size_t hlen = font->tables[i].length;
            uint8_t *head_copy = (uint8_t *)malloc(hlen);
            if (!head_copy) { tspdf_buffer_destroy(&new_glyf); tspdf_buffer_destroy(&new_loca); free(tables); return false; }
            memcpy(head_copy, font->data + font->tables[i].offset, hlen);
            // indexToLocFormat is at offset 50 in head table
            if (hlen >= 52) {
                int16_t new_format = use_long_loca ? 1 : 0;
                w16(head_copy + 50, (uint16_t)new_format);
            }
            // Zero out checksum adjustment (offset 8 in head) — we'll fix later
            if (hlen >= 12) {
                w32(head_copy + 8, 0);
            }
            t->data = head_copy;
            t->len = hlen;
            t->owned = true;
        } else {
            t->data = font->data + font->tables[i].offset;
            t->len = font->tables[i].length;
            t->owned = false;
        }
    }

    // --- Sort tables by tag (recommended by TrueType spec) ---
    qsort(tables, (size_t)table_count, sizeof(TableOut), table_out_cmp);

    // --- Calculate sizes ---
    // searchRange, entrySelector, rangeShift for table directory
    uint16_t search_range = 1;
    uint16_t entry_selector = 0;
    while (search_range * 2 <= (uint16_t)table_count) {
        search_range *= 2;
        entry_selector++;
    }
    search_range *= 16;
    uint16_t range_shift = (uint16_t)table_count * 16 - search_range;

    size_t header_size = 12 + table_count * 16;
    size_t total_size = header_size;
    for (int i = 0; i < table_count; i++) {
        total_size += (tables[i].len + 3) & ~3;  // pad to 4-byte boundary
    }

    // --- Write output TTF ---
    uint8_t *out = (uint8_t *)calloc(1, total_size);
    if (!out) goto cleanup;

    // Offset table
    w32(out, 0x00010000);  // sfVersion
    w16(out + 4, (uint16_t)table_count);
    w16(out + 6, search_range);
    w16(out + 8, entry_selector);
    w16(out + 10, range_shift);

    // Write table data and fill directory
    size_t data_offset = header_size;
    for (int i = 0; i < table_count; i++) {
        uint8_t *dir_entry = out + 12 + i * 16;
        w32(dir_entry, tables[i].tag);
        w32(dir_entry + 4, table_checksum(tables[i].data, tables[i].len));
        w32(dir_entry + 8, (uint32_t)data_offset);
        w32(dir_entry + 12, (uint32_t)tables[i].len);

        memcpy(out + data_offset, tables[i].data, tables[i].len);
        // Pad to 4-byte boundary
        data_offset = (data_offset + tables[i].len + 3) & ~3;
    }

    // --- Fix head.checksumAdjustment ---
    // checkSumAdjustment = 0xB1B0AFBA - checksum_of_entire_file
    uint32_t file_checksum = table_checksum(out, total_size);
    // Find head table in output to patch
    for (int i = 0; i < table_count; i++) {
        if (tables[i].tag == tag_head) {
            uint32_t head_off = r32(out + 12 + i * 16 + 8);
            w32(out + head_off + 8, 0xB1B0AFBA - file_checksum);
            // Recompute head table checksum in directory
            uint32_t head_len = r32(out + 12 + i * 16 + 12);
            w32(out + 12 + i * 16 + 4, table_checksum(out + head_off, head_len));
            break;
        }
    }

    *out_len = total_size;

cleanup:
    for (int i = 0; i < table_count; i++) {
        if (tables[i].owned) free((void *)tables[i].data);
    }
    free(tables);
    tspdf_buffer_destroy(&new_glyf);
    tspdf_buffer_destroy(&new_loca);

    return out;
}
