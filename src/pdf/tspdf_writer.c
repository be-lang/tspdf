#include "tspdf_writer.h"
#include "../image/png_decoder.h"
#include "../compress/deflate.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

TspdfWriter *tspdf_writer_create(void) {
    TspdfWriter *doc = calloc(1, sizeof(TspdfWriter));
    if (!doc) return NULL;
    doc->writer = tspdf_raw_writer_create();
    doc->default_page_size = (TspdfPageSize){TSPDF_PAGE_A4_WIDTH, TSPDF_PAGE_A4_HEIGHT};

    // Allocate initial pages array
    doc->page_capacity = TSPDF_MAX_PAGES_INITIAL;
    doc->pages = calloc(doc->page_capacity, sizeof(TspdfPage));
    if (!doc->pages) { free(doc); return NULL; }

    // Pre-allocate catalog and pages objects
    doc->catalog_ref = tspdf_raw_writer_alloc_object(&doc->writer);
    doc->pages_ref = tspdf_raw_writer_alloc_object(&doc->writer);

    return doc;
}

void tspdf_writer_destroy(TspdfWriter *doc) {
    for (int i = 0; i < doc->page_count; i++) {
        tspdf_stream_destroy(&doc->pages[i].stream);
        free(doc->pages[i].annotations);
    }
    for (int i = 0; i < doc->font_count; i++) {
        free(doc->fonts[i].used_glyphs);
        free(doc->fonts[i].glyph_to_unicode);
    }
    for (int i = 0; i < doc->ttf_font_count; i++) {
        ttf_free(&doc->ttf_fonts[i]);
    }
    for (int i = 0; i < doc->image_count; i++) {
        free(doc->images[i].rgb_data);
        free(doc->images[i].alpha_data);
    }
    tspdf_raw_writer_destroy(&doc->writer);
    free(doc->pages);
    free(doc);
}

void tspdf_writer_set_page_size(TspdfWriter *doc, double width, double height) {
    doc->default_page_size = (TspdfPageSize){width, height};
}

void tspdf_writer_set_title(TspdfWriter *doc, const char *title) {
    snprintf(doc->metadata.title, sizeof(doc->metadata.title), "%s", title);
}

void tspdf_writer_set_author(TspdfWriter *doc, const char *author) {
    snprintf(doc->metadata.author, sizeof(doc->metadata.author), "%s", author);
}

void tspdf_writer_set_subject(TspdfWriter *doc, const char *subject) {
    snprintf(doc->metadata.subject, sizeof(doc->metadata.subject), "%s", subject);
}

void tspdf_writer_set_creator(TspdfWriter *doc, const char *creator) {
    snprintf(doc->metadata.creator, sizeof(doc->metadata.creator), "%s", creator);
}

void tspdf_writer_set_creation_date(TspdfWriter *doc, const char *date) {
    snprintf(doc->metadata.creation_date, sizeof(doc->metadata.creation_date), "%s", date);
}

const char *tspdf_writer_add_opacity(TspdfWriter *doc, double fill_opacity, double stroke_opacity) {
    if (doc->opacity_count >= TSPDF_MAX_OPACITY_STATES) return NULL;

    // Check if an identical state already exists (epsilon for float comparison)
    for (int i = 0; i < doc->opacity_count; i++) {
        if (fabs(doc->opacity_states[i].fill_opacity - fill_opacity) < 1e-6 &&
            fabs(doc->opacity_states[i].stroke_opacity - stroke_opacity) < 1e-6) {
            return doc->opacity_states[i].name;
        }
    }

    TspdfOpacityState *state = &doc->opacity_states[doc->opacity_count];
    state->fill_opacity = fill_opacity;
    state->stroke_opacity = stroke_opacity;
    state->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    snprintf(state->name, sizeof(state->name), "GS%d", doc->opacity_count + 1);
    doc->opacity_count++;

    return state->name;
}

const char *tspdf_writer_add_gradient(TspdfWriter *doc,
    double x0, double y0, double x1, double y1,
    TspdfColor color0, TspdfColor color1) {
    if (doc->gradient_count >= TSPDF_MAX_GRADIENTS) return NULL;

    TspdfGradient *grad = &doc->gradients[doc->gradient_count];
    grad->type = TSPDF_GRADIENT_LINEAR;
    grad->x0 = x0;
    grad->y0 = y0;
    grad->x1 = x1;
    grad->y1 = y1;
    grad->color0 = color0;
    grad->color1 = color1;
    grad->stop_count = 0;
    grad->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    snprintf(grad->name, sizeof(grad->name), "Sh%d", doc->gradient_count + 1);
    doc->gradient_count++;

    return grad->name;
}

const char *tspdf_writer_add_gradient_stops(TspdfWriter *doc,
    double x0, double y0, double x1, double y1,
    const TspdfGradientStop *stops, int stop_count) {
    if (doc->gradient_count >= TSPDF_MAX_GRADIENTS) return NULL;
    if (stop_count < 2 || stop_count > TSPDF_GRADIENT_MAX_STOPS) return NULL;

    TspdfGradient *grad = &doc->gradients[doc->gradient_count];
    grad->type = TSPDF_GRADIENT_LINEAR;
    grad->x0 = x0;
    grad->y0 = y0;
    grad->x1 = x1;
    grad->y1 = y1;
    grad->stop_count = stop_count;
    for (int i = 0; i < stop_count; i++) {
        grad->stops[i] = stops[i];
    }
    grad->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    snprintf(grad->name, sizeof(grad->name), "Sh%d", doc->gradient_count + 1);
    doc->gradient_count++;
    return grad->name;
}

const char *tspdf_writer_add_radial_gradient(TspdfWriter *doc,
    double cx, double cy, double r0, double r1,
    TspdfColor color0, TspdfColor color1) {
    if (doc->gradient_count >= TSPDF_MAX_GRADIENTS) return NULL;

    TspdfGradient *grad = &doc->gradients[doc->gradient_count];
    grad->type = TSPDF_GRADIENT_RADIAL;
    grad->x0 = cx;
    grad->y0 = cy;
    grad->x1 = cx;
    grad->y1 = cy;
    grad->r0 = r0;
    grad->r1 = r1;
    grad->color0 = color0;
    grad->color1 = color1;
    grad->stop_count = 0;
    grad->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    snprintf(grad->name, sizeof(grad->name), "Sh%d", doc->gradient_count + 1);
    doc->gradient_count++;
    return grad->name;
}

const char *tspdf_writer_add_radial_gradient_stops(TspdfWriter *doc,
    double cx, double cy, double r0, double r1,
    const TspdfGradientStop *stops, int stop_count) {
    if (doc->gradient_count >= TSPDF_MAX_GRADIENTS) return NULL;
    if (stop_count < 2 || stop_count > TSPDF_GRADIENT_MAX_STOPS) return NULL;

    TspdfGradient *grad = &doc->gradients[doc->gradient_count];
    grad->type = TSPDF_GRADIENT_RADIAL;
    grad->x0 = cx;
    grad->y0 = cy;
    grad->x1 = cx;
    grad->y1 = cy;
    grad->r0 = r0;
    grad->r1 = r1;
    grad->stop_count = stop_count;
    for (int i = 0; i < stop_count; i++) {
        grad->stops[i] = stops[i];
    }
    grad->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    snprintf(grad->name, sizeof(grad->name), "Sh%d", doc->gradient_count + 1);
    doc->gradient_count++;
    return grad->name;
}

TspdfError tspdf_writer_last_error(TspdfWriter *doc) {
    return doc ? doc->last_error : TSPDF_ERR_INVALID_ARG;
}

TspdfStream *tspdf_writer_add_page_sized(TspdfWriter *doc, double width, double height) {
    if (doc->page_count >= doc->page_capacity) {
        int new_cap = doc->page_capacity * 2;
        TspdfPage *new_pages = realloc(doc->pages, new_cap * sizeof(TspdfPage));
        if (!new_pages) { doc->last_error = TSPDF_ERR_ALLOC; return NULL; }
        memset(new_pages + doc->page_capacity, 0, (new_cap - doc->page_capacity) * sizeof(TspdfPage));
        doc->pages = new_pages;
        doc->page_capacity = new_cap;
    }

    TspdfPage *page = &doc->pages[doc->page_count++];
    page->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    page->contents_ref = tspdf_raw_writer_alloc_object(&doc->writer);
    page->size = (TspdfPageSize){width, height};
    page->stream = tspdf_stream_create();
    page->annotations = NULL;
    page->annotation_count = 0;
    page->annotation_capacity = 0;

    doc->last_error = TSPDF_OK;
    return &page->stream;
}

TspdfStream *tspdf_writer_add_page(TspdfWriter *doc) {
    return tspdf_writer_add_page_sized(doc,
        doc->default_page_size.width,
        doc->default_page_size.height);
}

const char *tspdf_writer_add_builtin_font(TspdfWriter *doc, const char *base_font_name) {
    if (doc->font_count >= TSPDF_MAX_FONTS) { doc->last_error = TSPDF_ERR_FONT_LIMIT; return NULL; }

    TspdfFont *font = &doc->fonts[doc->font_count];
    font->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    font->is_builtin = true;
    font->ttf = NULL;
    font->base14 = tspdf_base14_get(base_font_name);
    snprintf(font->name, sizeof(font->name), "F%d", doc->font_count + 1);
    snprintf(font->base_font_name, sizeof(font->base_font_name), "%s", base_font_name);
    doc->font_count++;

    // Write font object
    TspdfRawWriter *w = &doc->writer;
    tspdf_raw_writer_begin_object(w, font->ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "Font");
    tspdf_raw_write_dict_name_name(w, "Subtype", "Type1");
    tspdf_raw_write_dict_name_name(w, "BaseFont", base_font_name);
    if (tspdf_base14_is_latin(base_font_name)) {
        tspdf_raw_write_dict_name_name(w, "Encoding", "WinAnsiEncoding");
    }
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    doc->last_error = TSPDF_OK;
    return font->name;
}

const char *tspdf_writer_add_ttf_font_from_memory(TspdfWriter *doc, const uint8_t *data, size_t len) {
    if (doc->font_count >= TSPDF_MAX_FONTS) { doc->last_error = TSPDF_ERR_FONT_LIMIT; return NULL; }
    if (doc->ttf_font_count >= TSPDF_MAX_FONTS) { doc->last_error = TSPDF_ERR_FONT_LIMIT; return NULL; }

    // Copy data (ttf_load_from_memory takes ownership)
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) { doc->last_error = TSPDF_ERR_ALLOC; return NULL; }
    memcpy(copy, data, len);

    TTF_Font *ttf = &doc->ttf_fonts[doc->ttf_font_count];
    if (!ttf_load_from_memory(ttf, copy, len)) {
        doc->last_error = TSPDF_ERR_FONT_PARSE;
        return NULL;
    }
    doc->ttf_font_count++;

    TspdfFont *font = &doc->fonts[doc->font_count];
    font->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    font->is_builtin = false;
    font->ttf = ttf;
    font->base14 = NULL;
    font->used_glyphs = (bool *)calloc(ttf->num_glyphs, sizeof(bool));
    font->glyph_to_unicode = (uint32_t *)calloc(ttf->num_glyphs, sizeof(uint32_t));
    if (!font->used_glyphs || !font->glyph_to_unicode) {
        free(font->used_glyphs);
        free(font->glyph_to_unicode);
        font->used_glyphs = NULL;
        font->glyph_to_unicode = NULL;
        ttf_free(ttf);
        doc->ttf_font_count--;
        doc->last_error = TSPDF_ERR_ALLOC;
        return NULL;
    }
    font->used_glyphs_count = ttf->num_glyphs;
    snprintf(font->name, sizeof(font->name), "F%d", doc->font_count + 1);
    font->base_font_name[0] = '\0';
    doc->font_count++;

    doc->last_error = TSPDF_OK;
    return font->name;
}

const char *tspdf_writer_add_ttf_font(TspdfWriter *doc, const char *ttf_path) {
    if (doc->font_count >= TSPDF_MAX_FONTS) { doc->last_error = TSPDF_ERR_FONT_LIMIT; return NULL; }
    if (doc->ttf_font_count >= TSPDF_MAX_FONTS) { doc->last_error = TSPDF_ERR_FONT_LIMIT; return NULL; }

    // Load the TTF file
    TTF_Font *ttf = &doc->ttf_fonts[doc->ttf_font_count];
    if (!ttf_load(ttf, ttf_path)) {
        doc->last_error = TSPDF_ERR_FONT_PARSE;
        return NULL;
    }
    doc->ttf_font_count++;

    // Register in the font list (font objects written during save for subsetting)
    TspdfFont *font = &doc->fonts[doc->font_count];
    font->ref = tspdf_raw_writer_alloc_object(&doc->writer);  // reserve font dict ref
    font->is_builtin = false;
    font->ttf = ttf;
    font->base14 = NULL;
    font->used_glyphs = (bool *)calloc(ttf->num_glyphs, sizeof(bool));
    font->glyph_to_unicode = (uint32_t *)calloc(ttf->num_glyphs, sizeof(uint32_t));
    if (!font->used_glyphs || !font->glyph_to_unicode) {
        free(font->used_glyphs);
        free(font->glyph_to_unicode);
        font->used_glyphs = NULL;
        font->glyph_to_unicode = NULL;
        ttf_free(ttf);
        doc->ttf_font_count--;
        doc->last_error = TSPDF_ERR_ALLOC;
        return NULL;
    }
    font->used_glyphs_count = ttf->num_glyphs;
    snprintf(font->name, sizeof(font->name), "F%d", doc->font_count + 1);
    font->base_font_name[0] = '\0';
    doc->font_count++;

    doc->last_error = TSPDF_OK;
    return font->name;
}

// Write a TTF font's PDF objects (called during save, after usage tracking)
static void write_ttf_font_objects(TspdfWriter *doc, TspdfFont *font) {
    TTF_Font *ttf = font->ttf;
    if (ttf->units_per_em == 0) return;  // guard against malformed font
    TspdfRawWriter *w = &doc->writer;

    const char *ps_name = ttf->postscript_name[0] ? ttf->postscript_name : "CustomFont";

    // Allocate all object refs
    TspdfRef type0_ref = font->ref;  // what users reference as /Fn
    TspdfRef cidfont_ref = tspdf_raw_writer_alloc_object(w);
    TspdfRef descriptor_ref = tspdf_raw_writer_alloc_object(w);
    TspdfRef file_ref = tspdf_raw_writer_alloc_object(w);
    TspdfRef tounicode_ref = tspdf_raw_writer_alloc_object(w);

    // --- Build subset font ---
    int glyphs_used = 0;
    for (int g = 0; g < font->used_glyphs_count; g++) {
        if (font->used_glyphs[g]) glyphs_used++;
    }

    size_t embed_len = ttf->data_len;
    uint8_t *embed_data = ttf->data;
    uint8_t *subset_data = NULL;
    bool is_subset = false;

    if (ttf->loca_offset && ttf->glyf_offset && glyphs_used > 0) {
        size_t subset_len = 0;
        subset_data = ttf_subset(ttf, font->used_glyphs, &subset_len);
        if (subset_data) {
            embed_data = subset_data;
            embed_len = subset_len;
            is_subset = true;
        }
    }

    // Generate subset tag (6 uppercase letters + '+'), unique per font
    char subset_name[512];
    if (is_subset) {
        int idx = (int)(font - doc->fonts);
        char tag[7];
        for (int t = 0; t < 6; t++) {
            tag[t] = 'A' + ((idx * 7 + t * 13 + glyphs_used) % 26);
        }
        tag[6] = '\0';
        snprintf(subset_name, sizeof(subset_name), "%s+%s", tag, ps_name);
        ps_name = subset_name;
    }

    // --- FontFile2 (subset TTF stream) ---
    tspdf_raw_writer_begin_object(w, file_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_int(w, "Length1", (int)embed_len);
    tspdf_raw_write_stream_compressed(w, embed_data, embed_len);
    tspdf_raw_writer_end_object(w);

    if (subset_data) free(subset_data);

    // --- FontDescriptor ---
    tspdf_raw_writer_begin_object(w, descriptor_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "FontDescriptor");
    tspdf_raw_write_dict_name_name(w, "FontName", ps_name);
    tspdf_raw_write_dict_name_int(w, "Flags", 32);  // Nonsymbolic

    tspdf_raw_write_name(w, "FontBBox");
    tspdf_raw_write_array_begin(w);
    tspdf_raw_write_int(w, (int)((double)ttf->x_min * 1000.0 / ttf->units_per_em));
    tspdf_raw_write_int(w, (int)((double)ttf->y_min * 1000.0 / ttf->units_per_em));
    tspdf_raw_write_int(w, (int)((double)ttf->x_max * 1000.0 / ttf->units_per_em));
    tspdf_raw_write_int(w, (int)((double)ttf->y_max * 1000.0 / ttf->units_per_em));
    tspdf_raw_write_array_end(w);

    tspdf_raw_write_dict_name_real(w, "ItalicAngle", 0.0);
    tspdf_raw_write_dict_name_int(w, "Ascent",
        (int)((double)ttf->ascent * 1000.0 / ttf->units_per_em));
    tspdf_raw_write_dict_name_int(w, "Descent",
        (int)((double)ttf->descent * 1000.0 / ttf->units_per_em));

    int16_t cap_height = ttf->has_os2 && ttf->os2_cap_height ?
        ttf->os2_cap_height : (int16_t)(ttf->ascent * 0.7);
    tspdf_raw_write_dict_name_int(w, "CapHeight",
        (int)((double)cap_height * 1000.0 / ttf->units_per_em));

    tspdf_raw_write_dict_name_int(w, "StemV", 80);
    tspdf_raw_write_dict_name_ref(w, "FontFile2", file_ref);
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    // --- ToUnicode CMap ---
    // Maps glyph IDs to Unicode codepoints from font->glyph_to_unicode[]
    TspdfBuffer cmap = tspdf_buffer_create(4096);
    tspdf_buffer_append_str(&cmap,
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo\n"
        "<< /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n"
    );

    {
        // Heap-allocate mapping arrays (too large for stack)
        int max_mappings = font->used_glyphs_count;
        uint16_t *glyph_ids = malloc(max_mappings * sizeof(uint16_t));
        uint32_t *unicode_vals = malloc(max_mappings * sizeof(uint32_t));
        int mapping_count = 0;

        if (glyph_ids && unicode_vals) {
            for (int g = 0; g < font->used_glyphs_count; g++) {
                if (!font->used_glyphs[g]) continue;
                if (!font->glyph_to_unicode) continue;
                uint32_t ucp = font->glyph_to_unicode[g];
                if (ucp == 0) continue;
                glyph_ids[mapping_count] = (uint16_t)g;
                unicode_vals[mapping_count] = ucp;
                mapping_count++;
            }

            // Write in batches of 100 (PDF CMap limit per beginbfchar block)
            int offset = 0;
            while (offset < mapping_count) {
                int batch = mapping_count - offset;
                if (batch > 100) batch = 100;
                char tmp[80];
                snprintf(tmp, sizeof(tmp), "%d beginbfchar\n", batch);
                tspdf_buffer_append_str(&cmap, tmp);
                for (int i = 0; i < batch; i++) {
                    uint32_t ucp = unicode_vals[offset + i];
                    if (ucp > 0xFFFF) {
                        // UTF-16 surrogate pair
                        uint32_t u = ucp - 0x10000;
                        uint16_t hi = 0xD800 + (uint16_t)(u >> 10);
                        uint16_t lo = 0xDC00 + (uint16_t)(u & 0x3FF);
                        snprintf(tmp, sizeof(tmp), "<%04X> <%04X%04X>\n",
                                 glyph_ids[offset + i], hi, lo);
                    } else {
                        snprintf(tmp, sizeof(tmp), "<%04X> <%04X>\n",
                                 glyph_ids[offset + i], (uint16_t)ucp);
                    }
                    tspdf_buffer_append_str(&cmap, tmp);
                }
                tspdf_buffer_append_str(&cmap, "endbfchar\n");
                offset += batch;
            }
        }

        free(glyph_ids);
        free(unicode_vals);
    }

    tspdf_buffer_append_str(&cmap,
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n"
    );

    tspdf_raw_writer_begin_object(w, tounicode_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_stream_compressed(w, cmap.data, cmap.len);
    tspdf_raw_writer_end_object(w);
    tspdf_buffer_destroy(&cmap);

    // --- CIDFont dict ---
    tspdf_raw_writer_begin_object(w, cidfont_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "Font");
    tspdf_raw_write_dict_name_name(w, "Subtype", "CIDFontType2");
    tspdf_raw_write_dict_name_name(w, "BaseFont", ps_name);

    tspdf_raw_write_name(w, "CIDSystemInfo");
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_string(w, "Registry", "Adobe");
    tspdf_raw_write_dict_name_string(w, "Ordering", "Identity");
    tspdf_raw_write_dict_name_int(w, "Supplement", 0);
    tspdf_raw_write_dict_end(w);

    // Default width
    tspdf_raw_write_dict_name_int(w, "DW", 1000);

    // W array — group consecutive used glyph IDs
    tspdf_raw_write_name(w, "W");
    tspdf_raw_write_array_begin(w);
    {
        int prev_g = -2;
        bool in_array = false;
        for (int g = 0; g < font->used_glyphs_count; g++) {
            if (!font->used_glyphs[g]) continue;
            if (g != prev_g + 1) {
                if (in_array) tspdf_raw_write_array_end(w);
                tspdf_raw_write_int(w, g);
                tspdf_raw_write_array_begin(w);
                in_array = true;
            }
            uint16_t advance = ttf_get_glyph_advance(ttf, (uint16_t)g);
            int width = (int)((double)advance * 1000.0 / ttf->units_per_em);
            tspdf_raw_write_int(w, width);
            prev_g = g;
        }
        if (in_array) tspdf_raw_write_array_end(w);
    }
    tspdf_raw_write_array_end(w);

    tspdf_raw_write_dict_name_name(w, "CIDToGIDMap", "Identity");
    tspdf_raw_write_dict_name_ref(w, "FontDescriptor", descriptor_ref);
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    // --- Type0 font dict ---
    tspdf_raw_writer_begin_object(w, type0_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "Font");
    tspdf_raw_write_dict_name_name(w, "Subtype", "Type0");
    tspdf_raw_write_dict_name_name(w, "BaseFont", ps_name);
    tspdf_raw_write_dict_name_name(w, "Encoding", "Identity-H");
    tspdf_raw_write_name(w, "DescendantFonts");
    tspdf_raw_write_array_begin(w);
    tspdf_raw_write_ref(w, cidfont_ref);
    tspdf_raw_write_array_end(w);
    tspdf_raw_write_dict_name_ref(w, "ToUnicode", tounicode_ref);
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);
}

const char *tspdf_writer_add_jpeg_image(TspdfWriter *doc, const char *jpeg_path) {
    if (doc->image_count >= TSPDF_MAX_IMAGES) { doc->last_error = TSPDF_ERR_IMAGE_LIMIT; return NULL; }

    JpegImage jpeg;
    if (!jpeg_load(&jpeg, jpeg_path)) {
        doc->last_error = TSPDF_ERR_IMAGE_PARSE;
        return NULL;
    }

    TspdfImage *img = &doc->images[doc->image_count];
    img->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    img->width = jpeg.width;
    img->height = jpeg.height;
    snprintf(img->name, sizeof(img->name), "Im%d", doc->image_count + 1);
    doc->image_count++;

    // Write image XObject with DCTDecode filter (raw JPEG data)
    TspdfRawWriter *w = &doc->writer;
    tspdf_raw_writer_begin_object(w, img->ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "XObject");
    tspdf_raw_write_dict_name_name(w, "Subtype", "Image");
    tspdf_raw_write_dict_name_int(w, "Width", jpeg.width);
    tspdf_raw_write_dict_name_int(w, "Height", jpeg.height);
    tspdf_raw_write_dict_name_name(w, "Filter", "DCTDecode");
    tspdf_raw_write_dict_name_int(w, "BitsPerComponent", 8);

    if (jpeg.components == 1) {
        tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceGray");
    } else if (jpeg.components == 4) {
        tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceCMYK");
    } else {
        tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceRGB");
    }

    tspdf_raw_write_stream(w, jpeg.data, jpeg.data_len);
    tspdf_raw_writer_end_object(w);

    jpeg_free(&jpeg);
    doc->last_error = TSPDF_OK;
    return img->name;
}

const char *tspdf_writer_add_png_image(TspdfWriter *doc, const char *png_path) {
    if (doc->image_count >= TSPDF_MAX_IMAGES) { doc->last_error = TSPDF_ERR_IMAGE_LIMIT; return NULL; }

    PngImage png;
    if (!png_image_load(png_path, &png)) {
        doc->last_error = TSPDF_ERR_IMAGE_PARSE;
        return NULL;
    }

    TspdfImage *img = &doc->images[doc->image_count++];
    img->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    img->width = png.width;
    img->height = png.height;
    img->is_png = true;
    snprintf(img->name, sizeof(img->name), "Im%d", doc->image_count);

    // Separate RGB and alpha channels
    // Guard against integer overflow: check before multiplication
    if (png.width <= 0 || png.height <= 0 ||
        (size_t)png.height > SIZE_MAX / (size_t)png.width) {
        png_image_free(&png);
        doc->image_count--;
        doc->last_error = TSPDF_ERR_IMAGE_PARSE;
        return NULL;
    }
    size_t pixel_count = (size_t)png.width * (size_t)png.height;
    if (pixel_count > (size_t)100000000) {
        png_image_free(&png);
        doc->image_count--;
        doc->last_error = TSPDF_ERR_IMAGE_PARSE;
        return NULL;
    }
    img->rgb_data = (uint8_t *)malloc(pixel_count * 3);
    if (!img->rgb_data) { png_image_free(&png); doc->image_count--; doc->last_error = TSPDF_ERR_ALLOC; return NULL; }
    img->rgb_len = pixel_count * 3;

    bool has_alpha = (png.channels == 4);
    if (has_alpha) {
        img->alpha_data = (uint8_t *)malloc(pixel_count);
        if (!img->alpha_data) { free(img->rgb_data); img->rgb_data = NULL; png_image_free(&png); doc->image_count--; doc->last_error = TSPDF_ERR_ALLOC; return NULL; }
        img->alpha_len = pixel_count;
        img->smask_ref = tspdf_raw_writer_alloc_object(&doc->writer);
    } else {
        img->alpha_data = NULL;
        img->alpha_len = 0;
        img->smask_ref = (TspdfRef){0, 0};
    }

    for (size_t i = 0; i < pixel_count; i++) {
        img->rgb_data[i * 3 + 0] = png.pixels[i * png.channels + 0];
        img->rgb_data[i * 3 + 1] = png.pixels[i * png.channels + 1];
        img->rgb_data[i * 3 + 2] = png.pixels[i * png.channels + 2];
        if (has_alpha) {
            img->alpha_data[i] = png.pixels[i * 4 + 3];
        }
    }

    png_image_free(&png);

    // Write image XObject (RGB data, compressed)
    TspdfRawWriter *w = &doc->writer;
    tspdf_raw_writer_begin_object(w, img->ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "XObject");
    tspdf_raw_write_dict_name_name(w, "Subtype", "Image");
    tspdf_raw_write_dict_name_int(w, "Width", img->width);
    tspdf_raw_write_dict_name_int(w, "Height", img->height);
    tspdf_raw_write_dict_name_int(w, "BitsPerComponent", 8);
    tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceRGB");
    if (has_alpha) {
        tspdf_raw_write_dict_name_ref(w, "SMask", img->smask_ref);
    }
    tspdf_raw_write_stream_compressed(w, img->rgb_data, img->rgb_len);
    tspdf_raw_writer_end_object(w);

    // Write soft mask (alpha channel) if present
    if (has_alpha) {
        tspdf_raw_writer_begin_object(w, img->smask_ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_dict_name_name(w, "Type", "XObject");
        tspdf_raw_write_dict_name_name(w, "Subtype", "Image");
        tspdf_raw_write_dict_name_int(w, "Width", img->width);
        tspdf_raw_write_dict_name_int(w, "Height", img->height);
        tspdf_raw_write_dict_name_int(w, "BitsPerComponent", 8);
        tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceGray");
        tspdf_raw_write_stream_compressed(w, img->alpha_data, img->alpha_len);
        tspdf_raw_writer_end_object(w);
    }

    // Free pixel data (already written to PDF)
    free(img->rgb_data);
    img->rgb_data = NULL;
    free(img->alpha_data);
    img->alpha_data = NULL;

    doc->last_error = TSPDF_OK;
    return img->name;
}

TspdfError tspdf_writer_add_link(TspdfWriter *doc, int page_index, double x, double y, double w, double h, const char *url) {
    if (page_index < 0 || page_index >= doc->page_count) { doc->last_error = TSPDF_ERR_INVALID_ARG; return TSPDF_ERR_INVALID_ARG; }
    TspdfPage *page = &doc->pages[page_index];

    // Grow annotations array dynamically
    if (page->annotation_count >= page->annotation_capacity) {
        int new_cap = page->annotation_capacity == 0 ? 4 : page->annotation_capacity * 2;
        TspdfAnnotation *new_arr = realloc(page->annotations, new_cap * sizeof(TspdfAnnotation));
        if (!new_arr) { doc->last_error = TSPDF_ERR_ALLOC; return TSPDF_ERR_ALLOC; }
        page->annotations = new_arr;
        page->annotation_capacity = new_cap;
    }

    TspdfAnnotation *annot = &page->annotations[page->annotation_count++];
    annot->x = x;
    annot->y = y;
    annot->w = w;
    annot->h = h;
    snprintf(annot->url, sizeof(annot->url), "%s", url);
    annot->ref.id = 0; // allocated later during save
    doc->last_error = TSPDF_OK;
    return TSPDF_OK;
}

int tspdf_writer_add_bookmark(TspdfWriter *doc, const char *title, int page_index) {
    if (doc->bookmark_count >= TSPDF_MAX_BOOKMARKS) { doc->last_error = TSPDF_ERR_OVERFLOW; return -1; }

    int idx = doc->bookmark_count++;
    TspdfBookmark *bm = &doc->bookmarks[idx];
    snprintf(bm->title, sizeof(bm->title), "%s", title);
    bm->page_index = page_index;
    bm->parent = -1;
    bm->first_child = -1;
    bm->last_child = -1;
    bm->next = -1;
    bm->prev = -1;
    bm->ref.id = 0;

    return idx;
}

int tspdf_writer_add_child_bookmark(TspdfWriter *doc, int parent_index, const char *title, int page_index) {
    if (doc->bookmark_count >= TSPDF_MAX_BOOKMARKS) { doc->last_error = TSPDF_ERR_OVERFLOW; return -1; }
    if (parent_index < 0 || parent_index >= doc->bookmark_count) { doc->last_error = TSPDF_ERR_INVALID_ARG; return -1; }

    int idx = doc->bookmark_count++;
    TspdfBookmark *bm = &doc->bookmarks[idx];
    snprintf(bm->title, sizeof(bm->title), "%s", title);
    bm->page_index = page_index;
    bm->parent = parent_index;
    bm->first_child = -1;
    bm->last_child = -1;
    bm->next = -1;
    bm->prev = -1;
    bm->ref.id = 0;

    TspdfBookmark *parent = &doc->bookmarks[parent_index];
    if (parent->first_child == -1) {
        parent->first_child = idx;
        parent->last_child = idx;
    } else {
        // Link after the current last child
        int last = parent->last_child;
        doc->bookmarks[last].next = idx;
        bm->prev = last;
        parent->last_child = idx;
    }

    return idx;
}

TTF_Font *tspdf_writer_get_ttf(TspdfWriter *doc, const char *font_name) {
    for (int i = 0; i < doc->font_count; i++) {
        if (strcmp(doc->fonts[i].name, font_name) == 0) {
            return doc->fonts[i].ttf;
        }
    }
    return NULL;
}

TspdfFont *tspdf_writer_get_font(TspdfWriter *doc, const char *font_name) {
    for (int i = 0; i < doc->font_count; i++) {
        if (strcmp(doc->fonts[i].name, font_name) == 0) {
            return &doc->fonts[i];
        }
    }
    return NULL;
}

const TspdfBase14Metrics *tspdf_writer_get_base14(TspdfWriter *doc, const char *font_name) {
    for (int i = 0; i < doc->font_count; i++) {
        if (strcmp(doc->fonts[i].name, font_name) == 0)
            return doc->fonts[i].base14;
    }
    return NULL;
}

double tspdf_writer_measure_text(TspdfWriter *doc, const char *font_name, double font_size, const char *text) {
    // Check for TTF font first
    TTF_Font *ttf = tspdf_writer_get_ttf(doc, font_name);
    if (ttf) {
        int width_units = ttf_measure_string(ttf, text);
        return ttf_units_to_points(ttf, width_units, font_size);
    }
    // Fall back to base14 metrics for built-in fonts
    for (int i = 0; i < doc->font_count; i++) {
        if (strcmp(doc->fonts[i].name, font_name) == 0 && doc->fonts[i].base14) {
            return tspdf_base14_measure_text(doc->fonts[i].base14, font_size, text);
        }
    }
    return 0;
}

TspdfError tspdf_writer_add_text_field(TspdfWriter *doc, int page_index,
                                  const char *name, double x, double y, double w, double h,
                                  const char *default_value, const char *font_name, double font_size) {
    if (doc->form_field_count >= TSPDF_MAX_FORM_FIELDS) { doc->last_error = TSPDF_ERR_OVERFLOW; return TSPDF_ERR_OVERFLOW; }
    TspdfFormField *field = &doc->form_fields[doc->form_field_count++];
    memset(field, 0, sizeof(*field));
    field->type = TSPDF_FORM_TEXT;
    field->page_index = page_index;
    field->x = x;
    field->y = y;
    field->w = w;
    field->h = h;
    field->font_size = font_size;
    snprintf(field->name, sizeof(field->name), "%s", name);
    snprintf(field->default_value, sizeof(field->default_value), "%s", default_value ? default_value : "");
    snprintf(field->font_name, sizeof(field->font_name), "%s", font_name ? font_name : "Helvetica");
    field->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    doc->last_error = TSPDF_OK;
    return TSPDF_OK;
}

TspdfError tspdf_writer_add_checkbox(TspdfWriter *doc, int page_index,
                                const char *name, double x, double y, double size, bool checked) {
    if (doc->form_field_count >= TSPDF_MAX_FORM_FIELDS) { doc->last_error = TSPDF_ERR_OVERFLOW; return TSPDF_ERR_OVERFLOW; }
    TspdfFormField *field = &doc->form_fields[doc->form_field_count++];
    memset(field, 0, sizeof(*field));
    field->type = TSPDF_FORM_CHECKBOX;
    field->page_index = page_index;
    field->x = x;
    field->y = y;
    field->w = size;
    field->h = size;
    snprintf(field->name, sizeof(field->name), "%s", name);
    snprintf(field->default_value, sizeof(field->default_value), "%s", checked ? "Yes" : "Off");
    field->ref = tspdf_raw_writer_alloc_object(&doc->writer);
    doc->last_error = TSPDF_OK;
    return TSPDF_OK;
}

// Scan a content stream buffer for font usage: find /Fn Tf and (...) Tj patterns
static void scan_stream_for_char_usage(TspdfWriter *doc, const uint8_t *data, size_t len) {
    int current_font = -1;

    for (size_t i = 0; i < len; i++) {
        // Look for /Fn ... Tf (font selection) — same as before
        if (i + 2 < len && data[i] == '/' && data[i + 1] == 'F') {
            size_t j = i + 2;
            int font_num = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                font_num = font_num * 10 + (data[j] - '0');
                j++;
            }
            if (font_num > 0 && font_num <= doc->font_count) {
                size_t k = j;
                while (k < len && data[k] == ' ') k++;
                while (k < len && ((data[k] >= '0' && data[k] <= '9') || data[k] == '.' || data[k] == '-')) k++;
                while (k < len && data[k] == ' ') k++;
                if (k + 1 < len && data[k] == 'T' && data[k + 1] == 'f') {
                    current_font = font_num - 1;
                }
            }
        }

        // TTF fonts: parse <hex...> Tj
        if (data[i] == '<' && current_font >= 0 && !doc->fonts[current_font].is_builtin) {
            TspdfFont *font = &doc->fonts[current_font];
            size_t j = i + 1;
            while (j < len && data[j] != '>') {
                if (j + 3 < len) {
                    uint16_t glyph_id = 0;
                    bool valid = true;
                    for (int d = 0; d < 4 && valid; d++) {
                        uint8_t c = data[j + d];
                        uint8_t nibble;
                        if (c >= '0' && c <= '9') nibble = c - '0';
                        else if (c >= 'A' && c <= 'F') nibble = 10 + c - 'A';
                        else if (c >= 'a' && c <= 'f') nibble = 10 + c - 'a';
                        else { valid = false; break; }
                        glyph_id = (glyph_id << 4) | nibble;
                    }
                    if (valid && glyph_id < font->used_glyphs_count) {
                        font->used_glyphs[glyph_id] = true;
                    }
                    j += 4;
                } else {
                    break;
                }
            }
            if (j < len && data[j] == '>') i = j;
        }

        // Built-in fonts: skip (...) strings
        if (data[i] == '(' && current_font >= 0 && doc->fonts[current_font].is_builtin) {
            size_t j = i + 1;
            int depth = 1;
            while (j < len && depth > 0) {
                if (data[j] == '\\' && j + 1 < len) j++;
                else if (data[j] == '(') depth++;
                else if (data[j] == ')') depth--;
                j++;
            }
            i = j - 1;
        }
    }
}

TspdfError tspdf_writer_save(TspdfWriter *doc, const char *path) {
    if (doc->saved) {
        doc->last_error = TSPDF_ERR_INVALID_ARG;
        return TSPDF_ERR_INVALID_ARG;
    }
    doc->saved = true;
    TspdfRawWriter *w = &doc->writer;

    // Scan all page content streams for character usage (for font subsetting)
    for (int i = 0; i < doc->page_count; i++) {
        TspdfPage *page = &doc->pages[i];
        scan_stream_for_char_usage(doc, page->stream.buf.data, page->stream.buf.len);
    }

    // Write TTF font objects (deferred to here for subsetting)
    for (int i = 0; i < doc->font_count; i++) {
        if (!doc->fonts[i].is_builtin && doc->fonts[i].ttf) {
            write_ttf_font_objects(doc, &doc->fonts[i]);
        }
    }

    // Write content stream objects for each page
    for (int i = 0; i < doc->page_count; i++) {
        TspdfPage *page = &doc->pages[i];

        tspdf_raw_writer_begin_object(w, page->contents_ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_stream_compressed(w, page->stream.buf.data, page->stream.buf.len);
        tspdf_raw_writer_end_object(w);
    }

    // Allocate refs and write annotation objects for each page
    for (int i = 0; i < doc->page_count; i++) {
        TspdfPage *page = &doc->pages[i];
        for (int j = 0; j < page->annotation_count; j++) {
            TspdfAnnotation *annot = &page->annotations[j];
            annot->ref = tspdf_raw_writer_alloc_object(w);

            tspdf_raw_writer_begin_object(w, annot->ref);
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_name(w, "Type", "Annot");
            tspdf_raw_write_dict_name_name(w, "Subtype", "Link");
            // Rect: [x, y, x+w, y+h]
            tspdf_raw_write_name(w, "Rect");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_real(w, annot->x);
            tspdf_raw_write_real(w, annot->y);
            tspdf_raw_write_real(w, annot->x + annot->w);
            tspdf_raw_write_real(w, annot->y + annot->h);
            tspdf_raw_write_array_end(w);
            // No visible border
            tspdf_raw_write_name(w, "Border");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_int(w, 0);
            tspdf_raw_write_int(w, 0);
            tspdf_raw_write_int(w, 0);
            tspdf_raw_write_array_end(w);
            // Action: URI
            tspdf_raw_write_name(w, "A");
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_name(w, "S", "URI");
            tspdf_raw_write_dict_name_string(w, "URI", annot->url);
            tspdf_raw_write_dict_end(w);
            tspdf_raw_write_dict_end(w);
            tspdf_buffer_append_str(&w->output, "\n");
            tspdf_raw_writer_end_object(w);
        }
    }

    // Write form field widget annotation objects
    for (int i = 0; i < doc->form_field_count; i++) {
        TspdfFormField *field = &doc->form_fields[i];
        tspdf_raw_writer_begin_object(w, field->ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_dict_name_name(w, "Type", "Annot");
        tspdf_raw_write_dict_name_name(w, "Subtype", "Widget");

        // Rect
        tspdf_raw_write_name(w, "Rect");
        tspdf_raw_write_array_begin(w);
        tspdf_raw_write_real(w, field->x);
        tspdf_raw_write_real(w, field->y);
        tspdf_raw_write_real(w, field->x + field->w);
        tspdf_raw_write_real(w, field->y + field->h);
        tspdf_raw_write_array_end(w);

        // Field name
        tspdf_raw_write_dict_name_string(w, "T", field->name);

        if (field->type == TSPDF_FORM_TEXT) {
            tspdf_raw_write_dict_name_name(w, "FT", "Tx");  // text field
            if (field->default_value[0]) {
                tspdf_raw_write_dict_name_string(w, "V", field->default_value);
            }
            // Default appearance string: font + size + color
            char da[128];
            // Find font's internal name
            const char *int_name = field->font_name;
            for (int f = 0; f < doc->font_count; f++) {
                if (strcmp(doc->fonts[f].base_font_name, field->font_name) == 0 ||
                    strcmp(doc->fonts[f].name, field->font_name) == 0) {
                    int_name = doc->fonts[f].name;
                    break;
                }
            }
            snprintf(da, sizeof(da), "/%s %.1f Tf 0 0 0 rg", int_name, field->font_size);
            tspdf_raw_write_dict_name_string(w, "DA", da);

            // Border style
            tspdf_raw_write_name(w, "BS");
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_int(w, "W", 1);
            tspdf_raw_write_dict_name_name(w, "S", "S");  // solid
            tspdf_raw_write_dict_end(w);

            // Flags: none (editable)
            tspdf_raw_write_dict_name_int(w, "Ff", 0);
        } else if (field->type == TSPDF_FORM_CHECKBOX) {
            tspdf_raw_write_dict_name_name(w, "FT", "Btn");  // button field
            tspdf_raw_write_dict_name_string(w, "V", field->default_value);
            tspdf_raw_write_dict_name_string(w, "DA", "/ZaDb 0 Tf 0 0 0 rg");

            // Appearance dict for checkbox
            tspdf_raw_write_name(w, "AS");
            tspdf_raw_write_name(w, field->default_value);  // "Yes" or "Off"

            // MK: appearance characteristics
            tspdf_raw_write_name(w, "MK");
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_string(w, "CA", "4");  // checkmark character in ZapfDingbats
            tspdf_raw_write_dict_end(w);
        }

        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\n");
        tspdf_raw_writer_end_object(w);
    }

    // Write page objects
    for (int i = 0; i < doc->page_count; i++) {
        TspdfPage *page = &doc->pages[i];

        tspdf_raw_writer_begin_object(w, page->ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_dict_name_name(w, "Type", "Page");
        tspdf_raw_write_dict_name_ref(w, "Parent", doc->pages_ref);

        // MediaBox
        tspdf_raw_write_name(w, "MediaBox");
        tspdf_raw_write_array_begin(w);
        tspdf_raw_write_int(w, 0);
        tspdf_raw_write_int(w, 0);
        tspdf_raw_write_real(w, page->size.width);
        tspdf_raw_write_real(w, page->size.height);
        tspdf_raw_write_array_end(w);

        tspdf_raw_write_dict_name_ref(w, "Contents", page->contents_ref);

        // Resources
        if (doc->font_count > 0 || doc->opacity_count > 0 || doc->image_count > 0 || doc->gradient_count > 0) {
            tspdf_raw_write_name(w, "Resources");
            tspdf_raw_write_dict_begin(w);

            if (doc->font_count > 0) {
                tspdf_raw_write_name(w, "Font");
                tspdf_raw_write_dict_begin(w);
                for (int j = 0; j < doc->font_count; j++) {
                    tspdf_raw_write_name(w, doc->fonts[j].name);
                    tspdf_raw_write_ref(w, doc->fonts[j].ref);
                }
                tspdf_raw_write_dict_end(w);
            }

            if (doc->opacity_count > 0) {
                tspdf_raw_write_name(w, "ExtGState");
                tspdf_raw_write_dict_begin(w);
                for (int j = 0; j < doc->opacity_count; j++) {
                    tspdf_raw_write_name(w, doc->opacity_states[j].name);
                    tspdf_raw_write_ref(w, doc->opacity_states[j].ref);
                }
                tspdf_raw_write_dict_end(w);
            }

            if (doc->image_count > 0) {
                tspdf_raw_write_name(w, "XObject");
                tspdf_raw_write_dict_begin(w);
                for (int j = 0; j < doc->image_count; j++) {
                    tspdf_raw_write_name(w, doc->images[j].name);
                    tspdf_raw_write_ref(w, doc->images[j].ref);
                }
                tspdf_raw_write_dict_end(w);
            }

            if (doc->gradient_count > 0) {
                tspdf_raw_write_name(w, "Shading");
                tspdf_raw_write_dict_begin(w);
                for (int j = 0; j < doc->gradient_count; j++) {
                    tspdf_raw_write_name(w, doc->gradients[j].name);
                    tspdf_raw_write_ref(w, doc->gradients[j].ref);
                }
                tspdf_raw_write_dict_end(w);
            }

            tspdf_raw_write_dict_end(w);
        }

        // Annotations (hyperlinks + form fields)
        {
            // Count form fields on this page
            int form_count = 0;
            for (int j = 0; j < doc->form_field_count; j++) {
                if (doc->form_fields[j].page_index == i) form_count++;
            }
            if (page->annotation_count > 0 || form_count > 0) {
                tspdf_raw_write_name(w, "Annots");
                tspdf_raw_write_array_begin(w);
                for (int j = 0; j < page->annotation_count; j++) {
                    tspdf_raw_write_ref(w, page->annotations[j].ref);
                }
                for (int j = 0; j < doc->form_field_count; j++) {
                    if (doc->form_fields[j].page_index == i) {
                        tspdf_raw_write_ref(w, doc->form_fields[j].ref);
                    }
                }
                tspdf_raw_write_array_end(w);
            }
        }

        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\n");
        tspdf_raw_writer_end_object(w);
    }

    // Write Pages object
    tspdf_raw_writer_begin_object(w, doc->pages_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "Pages");
    tspdf_raw_write_dict_name_int(w, "Count", doc->page_count);

    tspdf_raw_write_name(w, "Kids");
    tspdf_raw_write_array_begin(w);
    for (int i = 0; i < doc->page_count; i++) {
        tspdf_raw_write_ref(w, doc->pages[i].ref);
    }
    tspdf_raw_write_array_end(w);

    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    // Write ExtGState objects
    for (int i = 0; i < doc->opacity_count; i++) {
        TspdfOpacityState *state = &doc->opacity_states[i];
        tspdf_raw_writer_begin_object(w, state->ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_dict_name_name(w, "Type", "ExtGState");
        tspdf_raw_write_dict_name_real(w, "ca", state->fill_opacity);
        tspdf_raw_write_dict_name_real(w, "CA", state->stroke_opacity);
        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\n");
        tspdf_raw_writer_end_object(w);
    }

    // Write Shading objects (gradients)
    for (int i = 0; i < doc->gradient_count; i++) {
        TspdfGradient *grad = &doc->gradients[i];
        tspdf_raw_writer_begin_object(w, grad->ref);
        tspdf_raw_write_dict_begin(w);

        // ShadingType: 2=axial (linear), 3=radial
        tspdf_raw_write_dict_name_int(w, "ShadingType", grad->type == TSPDF_GRADIENT_RADIAL ? 3 : 2);
        tspdf_raw_write_dict_name_name(w, "ColorSpace", "DeviceRGB");

        // Coords array
        tspdf_raw_write_name(w, "Coords");
        tspdf_raw_write_array_begin(w);
        if (grad->type == TSPDF_GRADIENT_RADIAL) {
            // [x0 y0 r0 x1 y1 r1]
            tspdf_raw_write_real(w, grad->x0);
            tspdf_raw_write_real(w, grad->y0);
            tspdf_raw_write_real(w, grad->r0);
            tspdf_raw_write_real(w, grad->x1);
            tspdf_raw_write_real(w, grad->y1);
            tspdf_raw_write_real(w, grad->r1);
        } else {
            tspdf_raw_write_real(w, grad->x0);
            tspdf_raw_write_real(w, grad->y0);
            tspdf_raw_write_real(w, grad->x1);
            tspdf_raw_write_real(w, grad->y1);
        }
        tspdf_raw_write_array_end(w);

        // Function
        tspdf_raw_write_name(w, "Function");

        if (grad->stop_count >= 2) {
            // Multi-stop: Type 3 stitching function with Type 2 sub-functions
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_int(w, "FunctionType", 3);
            tspdf_raw_write_name(w, "Domain");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_int(w, 0);
            tspdf_raw_write_int(w, 1);
            tspdf_raw_write_array_end(w);

            // Bounds: intermediate stop positions (excluding first and last)
            tspdf_raw_write_name(w, "Bounds");
            tspdf_raw_write_array_begin(w);
            for (int s = 1; s < grad->stop_count - 1; s++) {
                tspdf_raw_write_real(w, grad->stops[s].position);
            }
            tspdf_raw_write_array_end(w);

            // Encode: map each sub-range to [0,1]
            tspdf_raw_write_name(w, "Encode");
            tspdf_raw_write_array_begin(w);
            for (int s = 0; s < grad->stop_count - 1; s++) {
                tspdf_raw_write_int(w, 0);
                tspdf_raw_write_int(w, 1);
            }
            tspdf_raw_write_array_end(w);

            // Functions: one Type 2 per segment
            tspdf_raw_write_name(w, "Functions");
            tspdf_raw_write_array_begin(w);
            for (int s = 0; s < grad->stop_count - 1; s++) {
                TspdfColor c0 = grad->stops[s].color;
                TspdfColor c1 = grad->stops[s + 1].color;
                tspdf_raw_write_dict_begin(w);
                tspdf_raw_write_dict_name_int(w, "FunctionType", 2);
                tspdf_raw_write_name(w, "Domain");
                tspdf_raw_write_array_begin(w);
                tspdf_raw_write_int(w, 0);
                tspdf_raw_write_int(w, 1);
                tspdf_raw_write_array_end(w);
                tspdf_raw_write_name(w, "C0");
                tspdf_raw_write_array_begin(w);
                tspdf_raw_write_real(w, c0.r);
                tspdf_raw_write_real(w, c0.g);
                tspdf_raw_write_real(w, c0.b);
                tspdf_raw_write_array_end(w);
                tspdf_raw_write_name(w, "C1");
                tspdf_raw_write_array_begin(w);
                tspdf_raw_write_real(w, c1.r);
                tspdf_raw_write_real(w, c1.g);
                tspdf_raw_write_real(w, c1.b);
                tspdf_raw_write_array_end(w);
                tspdf_raw_write_dict_name_int(w, "N", 1);
                tspdf_raw_write_dict_end(w);
            }
            tspdf_raw_write_array_end(w);
            tspdf_raw_write_dict_end(w);
        } else {
            // Simple 2-stop: Type 2 exponential interpolation
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_int(w, "FunctionType", 2);
            tspdf_raw_write_name(w, "Domain");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_int(w, 0);
            tspdf_raw_write_int(w, 1);
            tspdf_raw_write_array_end(w);
            tspdf_raw_write_name(w, "C0");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_real(w, grad->color0.r);
            tspdf_raw_write_real(w, grad->color0.g);
            tspdf_raw_write_real(w, grad->color0.b);
            tspdf_raw_write_array_end(w);
            tspdf_raw_write_name(w, "C1");
            tspdf_raw_write_array_begin(w);
            tspdf_raw_write_real(w, grad->color1.r);
            tspdf_raw_write_real(w, grad->color1.g);
            tspdf_raw_write_real(w, grad->color1.b);
            tspdf_raw_write_array_end(w);
            tspdf_raw_write_dict_name_int(w, "N", 1);
            tspdf_raw_write_dict_end(w);
        }

        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\n");
        tspdf_raw_writer_end_object(w);
    }

    // Write Info dictionary (always included for Producer, plus any user-set metadata)
    TspdfRef info_ref = tspdf_raw_writer_alloc_object(w);
    tspdf_raw_writer_begin_object(w, info_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_string(w, "Producer", "tspdf");
    if (doc->metadata.title[0])
        tspdf_raw_write_dict_name_string(w, "Title", doc->metadata.title);
    if (doc->metadata.author[0])
        tspdf_raw_write_dict_name_string(w, "Author", doc->metadata.author);
    if (doc->metadata.subject[0])
        tspdf_raw_write_dict_name_string(w, "Subject", doc->metadata.subject);
    if (doc->metadata.creator[0])
        tspdf_raw_write_dict_name_string(w, "Creator", doc->metadata.creator);
    if (doc->metadata.creation_date[0])
        tspdf_raw_write_dict_name_string(w, "CreationDate", doc->metadata.creation_date);
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    // Write outline (bookmark) tree if any bookmarks exist
    TspdfRef outlines_ref = {0, 0};
    if (doc->bookmark_count > 0) {
        outlines_ref = tspdf_raw_writer_alloc_object(w);

        // Allocate refs for all bookmarks
        for (int i = 0; i < doc->bookmark_count; i++) {
            doc->bookmarks[i].ref = tspdf_raw_writer_alloc_object(w);
        }

        // Count children helper: count direct children of a parent (-1 = root level)
        // and find first/last among root-level bookmarks
        int root_first = -1, root_last = -1, root_count = 0;
        for (int i = 0; i < doc->bookmark_count; i++) {
            if (doc->bookmarks[i].parent == -1) {
                if (root_first == -1) root_first = i;
                root_last = i;
                root_count++;
            }
        }

        // Link root-level siblings (prev/next)
        int prev_root = -1;
        for (int i = 0; i < doc->bookmark_count; i++) {
            if (doc->bookmarks[i].parent == -1) {
                if (prev_root >= 0) {
                    doc->bookmarks[prev_root].next = i;
                    doc->bookmarks[i].prev = prev_root;
                }
                prev_root = i;
            }
        }

        // Write each bookmark object
        for (int i = 0; i < doc->bookmark_count; i++) {
            TspdfBookmark *bm = &doc->bookmarks[i];
            tspdf_raw_writer_begin_object(w, bm->ref);
            tspdf_raw_write_dict_begin(w);
            tspdf_raw_write_dict_name_string(w, "Title", bm->title);

            // Parent: either outline root or parent bookmark
            if (bm->parent == -1) {
                tspdf_raw_write_dict_name_ref(w, "Parent", outlines_ref);
            } else {
                tspdf_raw_write_dict_name_ref(w, "Parent", doc->bookmarks[bm->parent].ref);
            }

            // Sibling links
            if (bm->prev >= 0) {
                tspdf_raw_write_dict_name_ref(w, "Prev", doc->bookmarks[bm->prev].ref);
            }
            if (bm->next >= 0) {
                tspdf_raw_write_dict_name_ref(w, "Next", doc->bookmarks[bm->next].ref);
            }

            // Children
            if (bm->first_child >= 0) {
                tspdf_raw_write_dict_name_ref(w, "First", doc->bookmarks[bm->first_child].ref);
                tspdf_raw_write_dict_name_ref(w, "Last", doc->bookmarks[bm->last_child].ref);
                // Count children (negative = closed by default)
                int child_count = 0;
                int c = bm->first_child;
                while (c >= 0) {
                    child_count++;
                    c = doc->bookmarks[c].next;
                }
                tspdf_raw_write_dict_name_int(w, "Count", child_count);
            }

            // Destination: go to page, fit width
            if (bm->page_index >= 0 && bm->page_index < doc->page_count) {
                tspdf_raw_write_name(w, "Dest");
                tspdf_raw_write_array_begin(w);
                tspdf_raw_write_ref(w, doc->pages[bm->page_index].ref);
                tspdf_raw_write_name(w, "Fit");
                tspdf_raw_write_array_end(w);
            }

            tspdf_raw_write_dict_end(w);
            tspdf_buffer_append_str(&w->output, "\n");
            tspdf_raw_writer_end_object(w);
        }

        // Write outline root object
        tspdf_raw_writer_begin_object(w, outlines_ref);
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_dict_name_name(w, "Type", "Outlines");
        if (root_first >= 0) {
            tspdf_raw_write_dict_name_ref(w, "First", doc->bookmarks[root_first].ref);
            tspdf_raw_write_dict_name_ref(w, "Last", doc->bookmarks[root_last].ref);
            tspdf_raw_write_dict_name_int(w, "Count", root_count);
        }
        tspdf_raw_write_dict_end(w);
        tspdf_buffer_append_str(&w->output, "\n");
        tspdf_raw_writer_end_object(w);
    }

    // Write Catalog
    tspdf_raw_writer_begin_object(w, doc->catalog_ref);
    tspdf_raw_write_dict_begin(w);
    tspdf_raw_write_dict_name_name(w, "Type", "Catalog");
    tspdf_raw_write_dict_name_ref(w, "Pages", doc->pages_ref);
    if (outlines_ref.id > 0) {
        tspdf_raw_write_dict_name_ref(w, "Outlines", outlines_ref);
    }
    // AcroForm dictionary for interactive form fields
    if (doc->form_field_count > 0) {
        tspdf_raw_write_name(w, "AcroForm");
        tspdf_raw_write_dict_begin(w);
        // Field references
        tspdf_raw_write_name(w, "Fields");
        tspdf_raw_write_array_begin(w);
        for (int i = 0; i < doc->form_field_count; i++) {
            tspdf_raw_write_ref(w, doc->form_fields[i].ref);
        }
        tspdf_raw_write_array_end(w);
        // Default resources (fonts needed for text fields)
        tspdf_raw_write_name(w, "DR");
        tspdf_raw_write_dict_begin(w);
        tspdf_raw_write_name(w, "Font");
        tspdf_raw_write_dict_begin(w);
        for (int i = 0; i < doc->font_count; i++) {
            tspdf_raw_write_dict_name_ref(w, doc->fonts[i].name, doc->fonts[i].ref);
        }
        tspdf_raw_write_dict_end(w);
        tspdf_raw_write_dict_end(w);
        // NeedAppearances: let viewer generate appearance streams
        tspdf_raw_write_name(w, "NeedAppearances");
        tspdf_buffer_append_str(&w->output, " true");
        tspdf_raw_write_dict_end(w);
    }
    tspdf_raw_write_dict_end(w);
    tspdf_buffer_append_str(&w->output, "\n");
    tspdf_raw_writer_end_object(w);

    // Finish
    tspdf_raw_writer_finish(w, doc->catalog_ref, info_ref);

    if (!path) {
        // save_to_memory mode — caller reads from w->output directly
        doc->last_error = TSPDF_OK;
        return TSPDF_OK;
    }

    if (!tspdf_raw_writer_write_to_file(w, path)) {
        doc->last_error = TSPDF_ERR_IO;
        return TSPDF_ERR_IO;
    }
    doc->last_error = TSPDF_OK;
    return TSPDF_OK;
}

TspdfError tspdf_writer_save_to_memory(TspdfWriter *doc, uint8_t **out_data, size_t *out_len) {
    if (!doc || !out_data || !out_len) return TSPDF_ERR_INVALID_ARG;

    // Build the PDF into the writer's buffer (pass NULL path to skip file write)
    TspdfError err = tspdf_writer_save(doc, NULL);
    if (err != TSPDF_OK) return err;

    // Copy the buffer to caller-owned memory
    TspdfRawWriter *w = &doc->writer;
    *out_len = w->output.len;
    *out_data = (uint8_t *)malloc(w->output.len);
    if (!*out_data) { doc->last_error = TSPDF_ERR_ALLOC; return TSPDF_ERR_ALLOC; }
    memcpy(*out_data, w->output.data, w->output.len);

    return TSPDF_OK;
}
