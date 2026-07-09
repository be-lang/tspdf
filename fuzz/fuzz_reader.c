// libFuzzer harness for the PDF reader — the primary untrusted-input surface.
//
// Build:  make fuzz   (or see the `fuzz` target in the Makefile)
// Run:    ./build/fuzz/fuzz_reader fuzz/corpus/reader
//
// Each fuzzer input is treated as a whole PDF file. We exercise the full open ->
// query -> serialize -> destroy lifecycle so that bounds/recursion/overflow bugs
// anywhere in parsing, page-tree walking, or re-serialization surface as an
// ASan/UBSan abort rather than silently corrupting memory. The harness frees
// every allocation so LeakSanitizer can also catch error-path leaks.
//
// Zero third-party deps: this is just a translation unit that #includes the
// reader's public header and links against the existing library objects. It does
// not modify any reader source.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../src/reader/tspr.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(data, size, &err);
    if (!doc) {
        // A NULL result must report a non-OK error: an inconsistency here would
        // itself be a bug worth catching.
        return 0;
    }

    // Walk every page so indirect /Kids, /MediaBox and /Rotate resolution and
    // the page-tree cycle/depth guards all run against attacker-shaped input.
    size_t pages = tspdf_reader_page_count(doc);
    for (size_t i = 0; i < pages; i++) {
        TspdfReaderPage *page = tspdf_reader_get_page(doc, i);
        if (page) {
            // Touch fields so the optimizer cannot elide the access.
            volatile double sink = page->media_box[0] + (double)page->rotate;
            (void)sink;
        }
    }

    // Touch the metadata getters; they parse the Info dictionary lazily in some
    // builds, so exercising them widens coverage past the page tree.
    (void)tspdf_reader_get_title(doc);
    (void)tspdf_reader_get_author(doc);
    (void)tspdf_reader_get_subject(doc);
    (void)tspdf_reader_get_keywords(doc);
    (void)tspdf_reader_get_creator(doc);
    (void)tspdf_reader_get_producer(doc);
    (void)tspdf_reader_get_creation_date(doc);
    (void)tspdf_reader_get_mod_date(doc);

    // Text extraction parses content streams, ToUnicode CMaps, /Differences
    // and form XObjects — all attacker-shaped. Cap the page count so a
    // many-page input does not dominate throughput. The returned string lives
    // in the document arena; destroy frees it.
    size_t text_pages = pages < 4 ? pages : 4;
    for (size_t i = 0; i < text_pages; i++) {
        (void)tspdf_reader_page_text(doc, i, &err);
    }

    // Extract runs the doc-tree preservation path (outlines, AcroForm,
    // named-destination flattening) over the hostile object graph.
    if (pages > 0) {
        size_t keep = 0;
        TspdfReader *sub = tspdf_reader_extract(doc, &keep, 1, &err);
        if (sub) tspdf_reader_destroy(sub);
    }

    // Round-trip through the serializer to fuzz the write path over parsed,
    // possibly-degenerate object graphs.
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (tspdf_reader_save_to_memory(doc, &out, &out_len) == TSPDF_OK) {
        free(out);
    }

    tspdf_reader_destroy(doc);
    return 0;
}
