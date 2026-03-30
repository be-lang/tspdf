#include "ttf_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Big-endian reading helpers ---

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t read_i16(const uint8_t *p) {
    return (int16_t)read_u16(p);
}

static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// --- Table lookup ---

static const uint8_t *ttf_find_table(const TTF_Font *font, const char *tag_str, uint32_t *out_length) {
    uint32_t tag = make_tag(tag_str);
    for (int i = 0; i < font->num_tables; i++) {
        if (font->tables[i].tag == tag) {
            if (out_length) *out_length = font->tables[i].length;
            if (font->tables[i].offset + font->tables[i].length > font->data_len) return NULL;
            return font->data + font->tables[i].offset;
        }
    }
    return NULL;
}

// --- Parse individual tables ---

static bool parse_head(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "head", &len);
    if (!p || len < 54) return false;

    font->units_per_em = read_u16(p + 18);
    font->x_min = read_i16(p + 36);
    font->y_min = read_i16(p + 38);
    font->x_max = read_i16(p + 40);
    font->y_max = read_i16(p + 42);
    font->index_to_loc_format = read_i16(p + 50);
    return true;
}

static bool parse_hhea(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "hhea", &len);
    if (!p || len < 36) return false;

    font->ascent = read_i16(p + 4);
    font->descent = read_i16(p + 6);
    font->line_gap = read_i16(p + 8);
    font->num_h_metrics = read_u16(p + 34);
    return true;
}

static bool parse_maxp(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "maxp", &len);
    if (!p || len < 6) return false;

    font->num_glyphs = read_u16(p + 4);
    return true;
}

static bool parse_hmtx(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "hmtx", &len);
    if (!p) return false;

    font->h_metrics = (TTF_HMetric *)calloc(font->num_glyphs, sizeof(TTF_HMetric));
    if (!font->h_metrics) return false;

    // First num_h_metrics entries have both advance and lsb
    uint32_t offset = 0;
    uint16_t last_advance = 0;
    for (uint16_t i = 0; i < font->num_h_metrics && offset + 4 <= len; i++) {
        font->h_metrics[i].advance_width = read_u16(p + offset);
        font->h_metrics[i].lsb = read_i16(p + offset + 2);
        last_advance = font->h_metrics[i].advance_width;
        offset += 4;
    }

    // Remaining glyphs share the last advance width, only lsb differs
    for (uint16_t i = font->num_h_metrics; i < font->num_glyphs && offset + 2 <= len; i++) {
        font->h_metrics[i].advance_width = last_advance;
        font->h_metrics[i].lsb = read_i16(p + offset);
        offset += 2;
    }

    return true;
}

static bool parse_cmap(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "cmap", &len);
    if (!p || len < 4) return false;

    uint16_t num_subtables = read_u16(p + 2);

    // Priority: prefer full-Unicode subtables (format 12) over BMP-only (format 4)
    const uint8_t *best_full = NULL;   // platform 3/encoding 10, or platform 0/encoding 4
    const uint8_t *best_bmp = NULL;    // platform 3/encoding 1, or platform 0/encoding 3
    const uint8_t *fallback = NULL;    // any Unicode-ish table

    for (uint16_t i = 0; i < num_subtables; i++) {
        uint32_t rec = 4 + i * 8;
        if (rec + 8 > len) break;

        uint16_t platform = read_u16(p + rec);
        uint16_t encoding = read_u16(p + rec + 2);
        uint32_t subtable_offset = read_u32(p + rec + 4);

        if (subtable_offset >= len) continue;

        if (!best_full &&
            ((platform == 3 && encoding == 10) || (platform == 0 && encoding == 4))) {
            best_full = p + subtable_offset;
        }
        if (!best_bmp &&
            ((platform == 3 && encoding == 1) || (platform == 0 && encoding == 3))) {
            best_bmp = p + subtable_offset;
        }
        if (!fallback && (platform == 0 || (platform == 3 && encoding == 0))) {
            fallback = p + subtable_offset;
        }
    }

    const uint8_t *best = best_full ? best_full : (best_bmp ? best_bmp : fallback);
    if (!best) return false;

    font->cmap_format = read_u16(best);
    font->cmap_offset = (uint32_t)(best - font->data);
    font->cmap_length = (uint32_t)((p + len) - best);  // remaining bytes from subtable to end of cmap table
    return true;
}

static void parse_name_record(const TTF_Font *font __attribute__((unused)), const uint8_t *name_table,
                               uint32_t table_len, uint16_t name_id, char *out, size_t out_size,
                               uint16_t string_offset) {
    uint16_t count = read_u16(name_table + 2);
    const uint8_t *records = name_table + 6;

    for (uint16_t i = 0; i < count; i++) {
        if (6 + (uint32_t)(i + 1) * 12 > table_len) break;
        const uint8_t *rec = records + i * 12;
        uint16_t platform = read_u16(rec);
        uint16_t encoding = read_u16(rec + 2);
        uint16_t nid = read_u16(rec + 6);
        uint16_t slen = read_u16(rec + 8);
        uint16_t soff = read_u16(rec + 10);

        if (nid != name_id) continue;

        // Validate string bounds within the name table
        if ((uint32_t)string_offset + soff + slen > table_len) continue;

        const uint8_t *str = name_table + string_offset + soff;

        // Platform 3 (Windows) encoding 1 = UTF-16BE
        if (platform == 3 && encoding == 1) {
            size_t j = 0;
            for (uint16_t k = 0; k + 1 < slen && j < out_size - 1; k += 2) {
                uint16_t ch = read_u16(str + k);
                if (ch < 128) {
                    out[j++] = (char)ch;
                }
            }
            out[j] = '\0';
            return;
        }

        // Platform 1 (Mac) or platform 0 (Unicode)
        if (platform == 1 || platform == 0) {
            size_t copy = slen < out_size - 1 ? slen : out_size - 1;
            memcpy(out, str, copy);
            out[copy] = '\0';
            return;
        }
    }
}

static bool parse_name(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "name", &len);
    if (!p || len < 6) return false;

    uint16_t string_offset = read_u16(p + 4);

    parse_name_record(font, p, len, 1, font->font_family, sizeof(font->font_family), string_offset);
    parse_name_record(font, p, len, 2, font->font_subfamily, sizeof(font->font_subfamily), string_offset);
    parse_name_record(font, p, len, 4, font->full_name, sizeof(font->full_name), string_offset);
    parse_name_record(font, p, len, 6, font->postscript_name, sizeof(font->postscript_name), string_offset);

    return true;
}

static bool parse_os2(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "OS/2", &len);
    if (!p || len < 78) return false;

    font->has_os2 = true;
    font->os2_weight_class = read_u16(p + 4);
    font->os2_typo_ascender = read_i16(p + 68);
    font->os2_typo_descender = read_i16(p + 70);
    font->os2_typo_line_gap = read_i16(p + 72);

    if (len >= 88) {
        font->os2_x_height = read_i16(p + 86);
    }
    if (len >= 90) {
        font->os2_cap_height = read_i16(p + 88);
    }

    return true;
}

static int kern_pair_compare(const void *a, const void *b) {
    const TTF_KernPair *pa = (const TTF_KernPair *)a;
    const TTF_KernPair *pb = (const TTF_KernPair *)b;
    if (pa->left != pb->left) return (int)pa->left - (int)pb->left;
    return (int)pa->right - (int)pb->right;
}

static bool parse_kern(TTF_Font *font) {
    uint32_t len;
    const uint8_t *p = ttf_find_table(font, "kern", &len);
    if (!p || len < 4) return false;

    uint16_t version = read_u16(p);
    if (version != 0) return false;

    uint16_t n_tables = read_u16(p + 2);
    uint32_t offset = 4;

    // Count total pairs across all format 0 horizontal subtables
    int total_pairs = 0;
    uint32_t scan = offset;
    for (uint16_t t = 0; t < n_tables; t++) {
        if (scan + 6 > len) break;
        uint16_t sub_length = read_u16(scan + p + 2);
        uint16_t coverage = read_u16(scan + p + 4);
        uint8_t format = (coverage >> 8) & 0xFF;
        bool horizontal = (coverage & 0x01) != 0;
        if (format == 0 && horizontal) {
            if (scan + 8 > len) break;
            uint16_t n_pairs = read_u16(scan + p + 6);
            total_pairs += n_pairs;
        }
        scan += sub_length;
    }

    if (total_pairs == 0) return false;

    font->kern_pairs = (TTF_KernPair *)malloc(total_pairs * sizeof(TTF_KernPair));
    if (!font->kern_pairs) return false;

    int idx = 0;
    for (uint16_t t = 0; t < n_tables; t++) {
        if (offset + 6 > len) break;
        uint16_t sub_length = read_u16(p + offset + 2);
        uint16_t coverage = read_u16(p + offset + 4);
        uint8_t format = (coverage >> 8) & 0xFF;
        bool horizontal = (coverage & 0x01) != 0;

        if (format == 0 && horizontal) {
            if (offset + 14 > len) { offset += sub_length; continue; }
            uint16_t n_pairs = read_u16(p + offset + 6);
            // Skip searchRange(2), entrySelector(2), rangeShift(2) = 6 bytes after nPairs
            uint32_t pair_offset = offset + 14;

            for (uint16_t i = 0; i < n_pairs && idx < total_pairs; i++) {
                if (pair_offset + 6 > len) break;
                font->kern_pairs[idx].left  = read_u16(p + pair_offset);
                font->kern_pairs[idx].right = read_u16(p + pair_offset + 2);
                font->kern_pairs[idx].value = read_i16(p + pair_offset + 4);
                idx++;
                pair_offset += 6;
            }
        }
        offset += sub_length;
    }

    font->kern_pair_count = idx;
    font->has_kern = true;

    // Sort kern pairs by (left, right) for binary search lookup
    qsort(font->kern_pairs, (size_t)idx, sizeof(TTF_KernPair), kern_pair_compare);

    return true;
}

static void find_loca_glyf(TTF_Font *font) {
    for (int i = 0; i < font->num_tables; i++) {
        uint32_t tag = font->tables[i].tag;
        if (tag == make_tag("loca")) {
            font->loca_offset = font->tables[i].offset;
            font->loca_length = font->tables[i].length;
        } else if (tag == make_tag("glyf")) {
            font->glyf_offset = font->tables[i].offset;
            font->glyf_length = font->tables[i].length;
        }
    }
}

// --- Public API ---

bool ttf_load_from_memory(TTF_Font *font, uint8_t *data, size_t len) {
    memset(font, 0, sizeof(*font));
    font->data = data;
    font->data_len = len;

    if (len < 12) return false;

    // Read offset table
    uint32_t sfnt_version = read_u32(data);
    // Accept TrueType (0x00010000) and OpenType with TrueType outlines ('true')
    if (sfnt_version != 0x00010000 && sfnt_version != 0x74727565) {
        // Also accept OpenType CFF ('OTTO') for basic metrics
        if (sfnt_version != 0x4F54544F) return false;
    }

    font->num_tables = read_u16(data + 4);

    // Parse table directory
    font->tables = (TTF_TableEntry *)calloc(font->num_tables, sizeof(TTF_TableEntry));
    if (!font->tables) return false;

    for (int i = 0; i < font->num_tables; i++) {
        uint32_t offset = 12 + i * 16;
        if (offset + 16 > len) goto fail;
        font->tables[i].tag = read_u32(data + offset);
        font->tables[i].checksum = read_u32(data + offset + 4);
        font->tables[i].offset = read_u32(data + offset + 8);
        font->tables[i].length = read_u32(data + offset + 12);
    }

    // Parse required tables — on failure, clean up partially allocated resources.
    // Note: font->data is NOT freed here; the caller (ttf_load) owns it on failure.
    if (!parse_head(font)) goto fail;
    if (!parse_hhea(font)) goto fail;
    if (!parse_maxp(font)) goto fail;
    if (!parse_hmtx(font)) goto fail;
    if (!parse_cmap(font)) goto fail;

    // Parse optional tables
    parse_name(font);
    parse_os2(font);
    parse_kern(font);
    find_loca_glyf(font);

    return true;

fail:
    // Prevent ttf_free from freeing data — caller is responsible for it
    font->data = NULL;
    ttf_free(font);
    return false;
}

bool ttf_load(TTF_Font *font, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { fclose(f); return false; }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(f); return false; }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) { free(data); return false; }

    if (!ttf_load_from_memory(font, data, (size_t)size)) {
        free(data);
        return false;
    }

    return true;
}

void ttf_free(TTF_Font *font) {
    free(font->data);
    free(font->tables);
    free(font->h_metrics);
    free(font->kern_pairs);
    memset(font, 0, sizeof(*font));
}

uint16_t ttf_get_glyph_index(const TTF_Font *font, uint32_t codepoint) {
    if (font->cmap_offset == 0) return 0;
    const uint8_t *subtable = font->data + font->cmap_offset;
    uint16_t format = read_u16(subtable);

    if (format == 4) {
        // Format 4: Segment mapping to delta values
        uint16_t seg_count_x2 = read_u16(subtable + 6);
        uint16_t seg_count = seg_count_x2 / 2;

        // Validate that all derived pointers fall within font data bounds
        // Layout: subtable+14 + endCodes(seg_count_x2) + pad(2) + startCodes(seg_count_x2)
        //         + idDelta(seg_count_x2) + idRangeOffset(seg_count_x2)
        if (subtable + 14 + (size_t)seg_count_x2 * 4 + 2 > font->data + font->data_len) {
            return 0;
        }

        const uint8_t *end_codes = subtable + 14;
        const uint8_t *start_codes = end_codes + seg_count_x2 + 2;
        const uint8_t *id_deltas = start_codes + seg_count_x2;
        const uint8_t *id_range_offsets = id_deltas + seg_count_x2;

        for (uint16_t i = 0; i < seg_count; i++) {
            uint16_t end_code = read_u16(end_codes + i * 2);
            if (codepoint > end_code) continue;

            uint16_t start_code = read_u16(start_codes + i * 2);
            if (codepoint < start_code) return 0;

            uint16_t id_range_offset = read_u16(id_range_offsets + i * 2);
            int16_t id_delta = read_i16(id_deltas + i * 2);

            if (id_range_offset == 0) {
                return (uint16_t)((codepoint + id_delta) & 0xFFFF);
            } else {
                uint32_t glyph_offset = (uint32_t)(id_range_offsets + i * 2 - font->data)
                                        + id_range_offset
                                        + (codepoint - start_code) * 2;
                if (glyph_offset + 2 > font->data_len) return 0;
                uint16_t glyph_id = read_u16(font->data + glyph_offset);
                if (glyph_id == 0) return 0;
                return (uint16_t)((glyph_id + id_delta) & 0xFFFF);
            }
        }
    } else if (format == 12) {
        // Format 12: Segmented coverage (32-bit)
        if (font->cmap_length < 16) return 0;
        uint32_t num_groups = read_u32(subtable + 12);
        if (font->cmap_length < 16 + (uint64_t)num_groups * 12) return 0;
        const uint8_t *groups = subtable + 16;
        for (uint32_t i = 0; i < num_groups; i++) {
            uint32_t start = read_u32(groups + i * 12);
            uint32_t end = read_u32(groups + i * 12 + 4);
            uint32_t glyph_start = read_u32(groups + i * 12 + 8);
            if (codepoint >= start && codepoint <= end) {
                return (uint16_t)(glyph_start + (codepoint - start));
            }
        }
    } else if (format == 6) {
        // Format 6: Trimmed table mapping
        uint16_t first_code = read_u16(subtable + 6);
        uint16_t entry_count = read_u16(subtable + 8);
        if (codepoint >= first_code && codepoint < first_code + entry_count) {
            return read_u16(subtable + 10 + (codepoint - first_code) * 2);
        }
    }

    return 0;  // .notdef
}

int16_t ttf_get_kerning(TTF_Font *font, uint16_t left_glyph, uint16_t right_glyph) {
    if (!font->has_kern || !font->kern_pairs) return 0;

    // Binary search: kern_pairs sorted by (left, right)
    TTF_KernPair key = { .left = left_glyph, .right = right_glyph, .value = 0 };
    TTF_KernPair *found = (TTF_KernPair *)bsearch(&key, font->kern_pairs,
        (size_t)font->kern_pair_count, sizeof(TTF_KernPair), kern_pair_compare);
    return found ? found->value : 0;
}

uint16_t ttf_get_glyph_advance(const TTF_Font *font, uint16_t glyph_index) {
    if (!font->h_metrics || glyph_index >= font->num_glyphs) return 0;
    return font->h_metrics[glyph_index].advance_width;
}

uint16_t ttf_get_char_advance(const TTF_Font *font, uint32_t codepoint) {
    uint16_t glyph = ttf_get_glyph_index(font, codepoint);
    return ttf_get_glyph_advance(font, glyph);
}

int ttf_measure_string(const TTF_Font *font, const char *text) {
    int width = 0;
    uint16_t prev_glyph = 0;
    bool have_prev = false;

    for (const char *p = text; *p; ) {
        uint32_t cp = (uint8_t)*p;
        // Basic UTF-8 decode
        if (cp < 0x80) {
            p++;
        } else if (cp < 0xE0) {
            if (!p[1]) break;
            cp = ((cp & 0x1F) << 6) | ((uint8_t)p[1] & 0x3F);
            p += 2;
        } else if (cp < 0xF0) {
            if (!p[1] || !p[2]) break;
            cp = ((cp & 0x0F) << 12) | (((uint8_t)p[1] & 0x3F) << 6) | ((uint8_t)p[2] & 0x3F);
            p += 3;
        } else {
            if (!p[1] || !p[2] || !p[3]) break;
            cp = ((cp & 0x07) << 18) | (((uint8_t)p[1] & 0x3F) << 12) |
                 (((uint8_t)p[2] & 0x3F) << 6) | ((uint8_t)p[3] & 0x3F);
            p += 4;
        }

        uint16_t glyph = ttf_get_glyph_index(font, cp);

        // Add kerning between consecutive glyphs
        if (have_prev && font->has_kern) {
            width += ttf_get_kerning((TTF_Font *)font, prev_glyph, glyph);
        }

        width += ttf_get_glyph_advance(font, glyph);
        prev_glyph = glyph;
        have_prev = true;
    }
    return width;
}

double ttf_units_to_points(const TTF_Font *font, int font_units, double font_size) {
    if (font->units_per_em == 0) return 0;
    return (double)font_units / (double)font->units_per_em * font_size;
}

double ttf_get_ascent(const TTF_Font *font, double font_size) {
    int16_t asc = font->has_os2 ? font->os2_typo_ascender : font->ascent;
    return ttf_units_to_points(font, asc, font_size);
}

double ttf_get_descent(const TTF_Font *font, double font_size) {
    int16_t desc = font->has_os2 ? font->os2_typo_descender : font->descent;
    return ttf_units_to_points(font, desc, font_size);  // negative value
}

double ttf_get_line_height(const TTF_Font *font, double font_size) {
    double asc = ttf_get_ascent(font, font_size);
    double desc = ttf_get_descent(font, font_size);
    int16_t gap = font->has_os2 ? font->os2_typo_line_gap : font->line_gap;
    double line_gap = ttf_units_to_points(font, gap, font_size);
    return asc - desc + line_gap;
}
