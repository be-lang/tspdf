#include "ops.h"

#include "../../include/tspdf_overlay.h"
#include "../util/pdftext.h"
#include "../reader/tspr.h"

#include <stdlib.h>
#include <string.h>
#define _USE_MATH_DEFINES
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

TspdfError tsops_watermark_text(TspdfReader *doc,
                                const TsopsWatermarkText *params,
                                TsopsWatermarkTextDetail *detail)
{
    const char *text = params->text;
    double opacity = params->opacity;
    double font_size = params->font_size;

    // The watermark is drawn with the built-in Helvetica font, which uses
    // WinAnsiEncoding (cp1252): re-encode the UTF-8 input up front so the
    // content stream carries cp1252 bytes instead of raw UTF-8 mojibake.
    char *wa_text = malloc(strlen(text) + 1);
    if (!wa_text) return TSPDF_ERR_ALLOC;

    uint32_t bad_cp = 0;
    int conv = tspdf_utf8_to_cp1252(text, wa_text, &bad_cp);
    if (conv != TSPDF_PDFTEXT_OK) {
        free(wa_text);
        if (conv == TSPDF_PDFTEXT_UNMAPPED) {
            if (detail) detail->bad_codepoint = bad_cp;
            return TSPDF_ERR_UNSUPPORTED;
        }
        return TSPDF_ERR_ENCODING;
    }

    TspdfError err = TSPDF_OK;
    size_t page_count = tspdf_reader_page_count(doc);

    for (size_t i = 0; i < page_count; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (!page) continue;

        // Center of the VISIBLE page: the MediaBox origin is not always
        // (0,0), so the center is ((x0+x1)/2, (y0+y1)/2), not (w/2, h/2).
        double cx = (page->media_box[0] + page->media_box[2]) / 2.0;
        double cy = (page->media_box[1] + page->media_box[3]) / 2.0;

        // Compensate a page-level /Rotate: the viewer turns the page
        // clockwise by that many degrees, so pre-rotate the stamp the other
        // way (45 + rotate CCW) to keep the diagonal upright as viewed.
        // Rotation is about the page center, so the stamp stays centered.
        int rot = ((page->rotate % 360) + 360) % 360;
        double angle_rad = (45.0 + (double)rot) * M_PI / 180.0;
        double cos_a = cos(angle_rad);
        double sin_a = sin(angle_rad);

        TspdfWriter *writer = tspdf_writer_create();
        if (!writer) continue;

        const char *font_name = tspdf_writer_add_builtin_font(writer, "Helvetica");
        if (!font_name) {
            tspdf_writer_destroy(writer);
            continue;
        }

        // Add opacity state
        const char *gs_name = tspdf_writer_add_opacity(writer, opacity, opacity);

        TspdfStream *stream = tspdf_page_begin_content(doc, i);
        if (!stream) {
            tspdf_writer_destroy(writer);
            continue;
        }

        tspdf_stream_save(stream);

        // Set opacity if we got a graphics state
        if (gs_name) {
            tspdf_stream_set_opacity(stream, gs_name);
        }

        // Set gray color for watermark
        TspdfColor gray = tspdf_color_rgb(0.7, 0.7, 0.7);
        tspdf_stream_set_fill_color(stream, gray);

        // Transform: translate to center, then rotate 45 degrees
        tspdf_stream_concat_matrix(stream, cos_a, sin_a, -sin_a, cos_a, cx, cy);

        // Estimate text width to center it (rough: ~0.5 * font_size per char for Helvetica)
        double text_width = (double)strlen(wa_text) * font_size * 0.5;

        tspdf_stream_begin_text(stream);
        tspdf_stream_set_font(stream, font_name, font_size);
        tspdf_stream_text_position(stream, -text_width / 2.0, -font_size / 3.0);
        tspdf_stream_show_text(stream, wa_text);
        tspdf_stream_end_text(stream);

        tspdf_stream_restore(stream);

        err = tspdf_page_end_content(doc, i, stream, writer);
        tspdf_writer_destroy(writer);
        if (err != TSPDF_OK) {
            free(wa_text);
            return err;
        }
    }

    free(wa_text);
    return TSPDF_OK;
}

bool tsops_metadata_set(TspdfReader *doc, const char *key, size_t key_len,
                        const char *value)
{
    static const struct {
        const char *name;
        void (*set)(TspdfReader *, const char *);
    } fields[] = {
        {"title", tspdf_reader_set_title},
        {"author", tspdf_reader_set_author},
        {"subject", tspdf_reader_set_subject},
        {"keywords", tspdf_reader_set_keywords},
        {"creator", tspdf_reader_set_creator},
        {"producer", tspdf_reader_set_producer},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (strlen(fields[i].name) == key_len &&
            strncmp(key, fields[i].name, key_len) == 0) {
            fields[i].set(doc, value);
            tspdf_reader_sync_xmp_metadata(doc);
            return true;
        }
    }
    return false;
}

TspdfSaveOptions tsops_unlock_save_options(void)
{
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.decrypt = true;
    return opts;
}
