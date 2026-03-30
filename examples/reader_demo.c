#include <stdio.h>
#include <stdlib.h>
#include "../include/tspdf.h"
#include "../include/tspdf_overlay.h"

int main(void) {
    TspdfError err;

    // ================================================================
    // PHASE 1: Page manipulation
    // ================================================================

    // Step 1: Generate a 5-page PDF with tspdf
    printf("=== Phase 1: Page Manipulation ===\n\n");
    printf("1. Creating 5-page PDF with tspdf...\n");
    {
        TspdfWriter *doc = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
        for (int i = 1; i <= 5; i++) {
            TspdfStream *page = tspdf_writer_add_page(doc);
            tspdf_stream_set_font(page, font, 48.0);
            tspdf_stream_move_to(page, 72, 400);
            char text[64];
            snprintf(text, sizeof(text), "Page %d of 5", i);
            tspdf_stream_show_text(page, text);
            tspdf_stream_set_font(page, font, 18.0);
            tspdf_stream_move_to(page, 72, 350);
            tspdf_stream_show_text(page, "(original document)");
        }
        tspdf_writer_save(doc, "demo_original.pdf");
        tspdf_writer_destroy(doc);
        printf("   -> demo_original.pdf (5 pages)\n");
    }

    // Step 2: Extract pages 1, 3, 5
    printf("\n2. Extracting pages 1, 3, 5...\n");
    TspdfReader *doc = tspdf_reader_open_file("demo_original.pdf", &err);
    if (!doc) {
        fprintf(stderr, "Failed to open: %s\n", tspdf_error_string(err));
        return 1;
    }
    printf("   Opened: %zu pages\n", tspdf_reader_page_count(doc));

    size_t extract_pages[] = {0, 2, 4};
    TspdfReader *extracted = tspdf_reader_extract(doc, extract_pages, 3, &err);
    tspdf_reader_save(extracted, "demo_extracted.pdf");
    printf("   -> demo_extracted.pdf (%zu pages)\n",
           tspdf_reader_page_count(extracted));

    // Step 3: Rotate page 1 by 90 degrees
    printf("\n3. Rotating first page 90 degrees...\n");
    size_t rot_pages[] = {0};
    TspdfReader *rotated = tspdf_reader_rotate(extracted, rot_pages, 1, 90, &err);
    tspdf_reader_save(rotated, "demo_rotated.pdf");
    printf("   -> demo_rotated.pdf (page 1 rotated 90)\n");

    // Step 4: Merge with a cover page
    printf("\n4. Creating cover page and merging...\n");
    {
        TspdfWriter *cover = tspdf_writer_create();
        const char *font = tspdf_writer_add_builtin_font(cover, "Helvetica");
        TspdfStream *page = tspdf_writer_add_page(cover);
        tspdf_stream_set_font(page, font, 36.0);
        tspdf_stream_move_to(page, 72, 500);
        tspdf_stream_show_text(page, "COVER PAGE");
        tspdf_stream_set_font(page, font, 18.0);
        tspdf_stream_move_to(page, 72, 450);
        tspdf_stream_show_text(page, "(added by merge)");
        tspdf_writer_save(cover, "demo_cover.pdf");
        tspdf_writer_destroy(cover);
    }

    TspdfReader *cover = tspdf_reader_open_file("demo_cover.pdf", &err);
    TspdfReader *merge_docs[] = {cover, rotated};
    TspdfReader *merged = tspdf_reader_merge(merge_docs, 2, &err);
    tspdf_reader_save(merged, "demo_merged.pdf");
    printf("   -> demo_merged.pdf (%zu pages: cover + extracted)\n",
           tspdf_reader_page_count(merged));

    // Step 5: Reorder — move cover to the end
    printf("\n5. Reordering: moving cover to end...\n");
    size_t new_order[] = {1, 2, 3, 0};
    TspdfReader *reordered = tspdf_reader_reorder(merged, new_order, 4, &err);
    tspdf_reader_save(reordered, "demo_reordered.pdf");
    printf("   -> demo_reordered.pdf (%zu pages)\n",
           tspdf_reader_page_count(reordered));

    tspdf_reader_destroy(reordered);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(cover);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);

    // ================================================================
    // PHASE 2: Encryption & Metadata
    // ================================================================

    printf("\n=== Phase 2: Encryption & Metadata ===\n\n");

    // Step 6: Set metadata on a PDF
    printf("6. Setting metadata...\n");
    doc = tspdf_reader_open_file("demo_original.pdf", &err);
    tspdf_reader_set_title(doc, "My Demo Document");
    tspdf_reader_set_author(doc, "tspr library");
    tspdf_reader_set_subject(doc, "Demonstrating PDF metadata");
    tspdf_reader_set_keywords(doc, "pdf, tspr, demo, metadata");
    tspdf_reader_save(doc, "demo_with_metadata.pdf");
    printf("   -> demo_with_metadata.pdf\n");
    printf("   Title:    %s\n", tspdf_reader_get_title(doc));
    printf("   Author:   %s\n", tspdf_reader_get_author(doc));
    printf("   Subject:  %s\n", tspdf_reader_get_subject(doc));
    printf("   Keywords: %s\n", tspdf_reader_get_keywords(doc));
    tspdf_reader_destroy(doc);

    // Step 7: Encrypt with AES-128 (password: "secret")
    printf("\n7. Encrypting with AES-128 (password: \"secret\")...\n");
    doc = tspdf_reader_open_file("demo_original.pdf", &err);
    err = tspdf_reader_save_encrypted(doc, "demo_encrypted_128.pdf",
                                        "secret", "owner123", 0xFFFFFFFC, 128);
    if (err == TSPDF_OK) {
        printf("   -> demo_encrypted_128.pdf (AES-128, user pass: \"secret\")\n");
    } else {
        printf("   ERROR: %s\n", tspdf_error_string(err));
    }
    tspdf_reader_destroy(doc);

    // Step 8: Try opening encrypted PDF without password
    printf("\n8. Opening encrypted PDF without password...\n");
    doc = tspdf_reader_open_file("demo_encrypted_128.pdf", &err);
    if (!doc) {
        printf("   Correctly rejected: %s\n", tspdf_error_string(err));
    }

    // Step 9: Open with wrong password
    printf("\n9. Opening with wrong password...\n");
    doc = tspdf_reader_open_file_with_password("demo_encrypted_128.pdf", "wrong", &err);
    if (!doc) {
        printf("   Correctly rejected: %s\n", tspdf_error_string(err));
    }

    // Step 10: Open with correct password
    printf("\n10. Opening with correct password...\n");
    doc = tspdf_reader_open_file_with_password("demo_encrypted_128.pdf", "secret", &err);
    if (doc) {
        printf("   Success! %zu pages\n", tspdf_reader_page_count(doc));

        // Step 11: Save unencrypted (unlock)
        printf("\n11. Saving unencrypted (unlocking)...\n");
        tspdf_reader_save(doc, "demo_unlocked.pdf");
        printf("   -> demo_unlocked.pdf (no password needed)\n");

        tspdf_reader_destroy(doc);
    }

    // Step 12: Encrypt with AES-256
    printf("\n12. Encrypting with AES-256 (password: \"strong\")...\n");
    doc = tspdf_reader_open_file("demo_original.pdf", &err);
    err = tspdf_reader_save_encrypted(doc, "demo_encrypted_256.pdf",
                                        "strong", "owner", 0xFFFFFFFC, 256);
    if (err == TSPDF_OK) {
        printf("   -> demo_encrypted_256.pdf (AES-256, user pass: \"strong\")\n");
    }
    tspdf_reader_destroy(doc);

    // Step 13: Open AES-256, extract pages, save unencrypted
    printf("\n13. Open encrypted, extract 2 pages, save unencrypted...\n");
    doc = tspdf_reader_open_file_with_password("demo_encrypted_256.pdf", "strong", &err);
    if (doc) {
        size_t pages[] = {0, 2};
        TspdfReader *ext = tspdf_reader_extract(doc, pages, 2, &err);
        if (ext) {
            tspdf_reader_save(ext, "demo_encrypted_extract.pdf");
            printf("   -> demo_encrypted_extract.pdf (%zu pages, unencrypted)\n",
                   tspdf_reader_page_count(ext));
            tspdf_reader_destroy(ext);
        }
        tspdf_reader_destroy(doc);
    }

    // ================================================================
    // PHASE 3: Content Overlay & Annotations
    // ================================================================

    printf("\n=== Phase 3: Content Overlay & Annotations ===\n\n");

    // Step 14: Overlay watermark text
    printf("14. Adding watermark to page 1...\n");
    doc = tspdf_reader_open_file("demo_original.pdf", &err);
    {
        TspdfWriter *res = tspdf_writer_create();
        const char *wm_font = tspdf_writer_add_builtin_font(res, "Helvetica-Bold");

        // Page center and text measurement for proper centering
        TspdfReaderPage *pg = tspdf_reader_get_page(doc, 0);
        double W = pg->media_box[2] - pg->media_box[0];
        double H = pg->media_box[3] - pg->media_box[1];
        double cx = W / 2.0, cy = H / 2.0;
        double wm_size = 72.0;
        double cos45 = 0.70710678, sin45 = 0.70710678;
        double tw = tspdf_writer_measure_text(res, wm_font, wm_size, "CONFIDENTIAL");
        if (tw <= 0) tw = 12 * wm_size * 0.6;  // fallback estimate

        TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
        // Light gray, rotated 45 degrees around page center, text centered
        tspdf_stream_set_fill_color(overlay, tspdf_color_from_u8(200, 200, 200));
        tspdf_stream_concat_matrix(overlay, cos45, sin45, -sin45, cos45, cx, cy);
        tspdf_stream_begin_text(overlay);
        tspdf_stream_set_font(overlay, wm_font, wm_size);
        tspdf_stream_text_position(overlay, -tw / 2.0, 0);
        tspdf_stream_show_text(overlay, "CONFIDENTIAL");
        tspdf_stream_end_text(overlay);
        tspdf_page_end_content(doc, 0, overlay, res);
        tspdf_writer_destroy(res);
    }
    tspdf_reader_save(doc, "demo_watermark.pdf");
    printf("   -> demo_watermark.pdf (page 1 has watermark)\n");

    // Step 15: Add annotations
    printf("\n15. Adding annotations...\n");
    tspdf_page_add_link(doc, 0, 72, 750, 200, 20, "https://github.com");
    tspdf_page_add_stamp(doc, 0, 350, 700, 150, 40, "Draft");
    tspdf_page_add_text_note(doc, 0, 300, 600, "Review", "Please review this page");
    tspdf_reader_save(doc, "demo_annotated.pdf");
    printf("   -> demo_annotated.pdf (link, stamp, text note)\n");
    tspdf_reader_destroy(doc);

    // Summary
    printf("\n=== Output Files ===\n\n");
    printf("  Page manipulation:\n");
    printf("    demo_original.pdf         - 5 pages\n");
    printf("    demo_extracted.pdf        - 3 pages (1, 3, 5)\n");
    printf("    demo_rotated.pdf          - 3 pages (page 1 rotated)\n");
    printf("    demo_merged.pdf           - 4 pages (cover + extracted)\n");
    printf("    demo_reordered.pdf        - 4 pages (cover moved to end)\n");
    printf("\n  Metadata:\n");
    printf("    demo_with_metadata.pdf    - has Title/Author/Subject/Keywords\n");
    printf("\n  Encryption:\n");
    printf("    demo_encrypted_128.pdf    - AES-128, password: \"secret\"\n");
    printf("    demo_encrypted_256.pdf    - AES-256, password: \"strong\"\n");
    printf("    demo_unlocked.pdf         - decrypted copy (no password)\n");
    printf("    demo_encrypted_extract.pdf - 2 pages extracted from encrypted\n");

    printf("\n  Content overlay & annotations:\n");
    printf("    demo_watermark.pdf        - page 1 has watermark text\n");
    printf("    demo_annotated.pdf        - link, stamp, and text note annotations\n");

    return 0;
}
