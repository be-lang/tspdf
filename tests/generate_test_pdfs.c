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

    printf("Test PDFs generated in tests/data/\n");
    return 0;
}
