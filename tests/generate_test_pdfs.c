#include <stdio.h>
#include "../src/pdf/tspdf_writer.h"
#include "../src/pdf/pdf_stream.h"

// Generates small test PDFs for tspr testing
int main(void) {
    // 1-page PDF
    {
        TspdfWriter *doc = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
        TspdfStream *page = tspdf_writer_add_page(doc);
        tspdf_stream_set_font(page, font, 24.0);
        tspdf_stream_move_to(page, 72, 720);
        tspdf_stream_show_text(page, "Page 1");
        tspdf_writer_save(doc, "tests/data/one_page.pdf");
        tspdf_writer_destroy(doc);
    }

    // 3-page PDF
    {
        TspdfWriter *doc = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
        for (int i = 1; i <= 3; i++) {
            TspdfStream *page = tspdf_writer_add_page(doc);
            tspdf_stream_set_font(page, font, 24.0);
            tspdf_stream_move_to(page, 72, 720);
            char text[32];
            snprintf(text, sizeof(text), "Page %d", i);
            tspdf_stream_show_text(page, text);
        }
        tspdf_writer_save(doc, "tests/data/three_pages.pdf");
        tspdf_writer_destroy(doc);
    }

    // 3-page PDF with an outline tree and form fields (doctree tests)
    {
        TspdfWriter *doc = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
        for (int i = 1; i <= 3; i++) {
            TspdfStream *page = tspdf_writer_add_page(doc);
            tspdf_stream_set_font(page, font, 24.0);
            tspdf_stream_move_to(page, 72, 720);
            char text[32];
            snprintf(text, sizeof(text), "Page %d", i);
            tspdf_stream_show_text(page, text);
        }
        int ch1 = tspdf_writer_add_bookmark(doc, "OF-CH1", 0);
        tspdf_writer_add_child_bookmark(doc, ch1, "OF-CH1-SUB", 1);
        tspdf_writer_add_bookmark(doc, "OF-CH2", 2);
        tspdf_writer_add_text_field(doc, 0, "of_text", 72, 600, 200, 20,
                                    "hello", "Helvetica", 12);
        tspdf_writer_add_checkbox(doc, 2, "of_check", 72, 560, 14, true);
        tspdf_writer_save(doc, "tests/data/outline_form.pdf");
        tspdf_writer_destroy(doc);
    }

    printf("Test PDFs generated in tests/data/\n");
    return 0;
}
