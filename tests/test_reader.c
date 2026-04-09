#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "test_framework.h"
#include "../src/reader/tspr.h"
#include "../src/reader/tspr_overlay.h"
#include "../src/reader/tspr_internal.h"
#include "../src/pdf/pdf_stream.h"
#include "../src/util/arena.h"

// --- Parser tests ---

TEST(test_parse_integer) {
    const char *input = "42";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_INT);
    ASSERT(obj->integer == 42);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_negative_integer) {
    const char *input = "-7";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_INT);
    ASSERT(obj->integer == -7);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_real) {
    const char *input = "3.14";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_REAL);
    ASSERT(obj->real > 3.13 && obj->real < 3.15);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_bool_true) {
    const char *input = "true";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_BOOL);
    ASSERT(obj->boolean == true);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_null) {
    const char *input = "null";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_NULL);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_name) {
    const char *input = "/Type";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)obj->string.data, "Type");
    tspdf_arena_destroy(&a);
}

TEST(test_parse_name_hex_escape) {
    const char *input = "/A#20B";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)obj->string.data, "A B");
    tspdf_arena_destroy(&a);
}

TEST(test_parse_literal_string) {
    const char *input = "(Hello World)";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STRING);
    ASSERT_EQ_SIZE(obj->string.len, 11);
    ASSERT(memcmp(obj->string.data, "Hello World", 11) == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_string_escape) {
    const char *input = "(Hello\\nWorld)";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STRING);
    ASSERT(obj->string.data[5] == '\n');
    tspdf_arena_destroy(&a);
}

TEST(test_parse_string_nested_parens) {
    const char *input = "(Hello (nested) World)";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STRING);
    ASSERT_EQ_SIZE(obj->string.len, 20);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_hex_string) {
    const char *input = "<48656C6C6F>";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STRING);
    ASSERT_EQ_SIZE(obj->string.len, 5);
    ASSERT(memcmp(obj->string.data, "Hello", 5) == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_array) {
    const char *input = "[1 2 /Name]";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(obj->array.count, 3);
    ASSERT_EQ_INT(obj->array.items[0].type, TSPDF_OBJ_INT);
    ASSERT(obj->array.items[0].integer == 1);
    ASSERT_EQ_INT(obj->array.items[2].type, TSPDF_OBJ_NAME);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_dict) {
    const char *input = "<< /Type /Page /Count 3 >>";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_DICT);
    ASSERT_EQ_SIZE(obj->dict.count, 2);
    TspdfObj *type_val = tspdf_dict_get(obj, "Type");
    ASSERT(type_val != NULL);
    ASSERT_EQ_INT(type_val->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)type_val->string.data, "Page");
    TspdfObj *count_val = tspdf_dict_get(obj, "Count");
    ASSERT(count_val != NULL);
    ASSERT(count_val->integer == 3);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_indirect_ref) {
    const char *input = "5 0 R";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_REF);
    ASSERT(obj->ref.num == 5);
    ASSERT(obj->ref.gen == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_indirect_obj) {
    const char *input = "1 0 obj\n<< /Type /Catalog >>\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num;
    uint16_t gen;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj != NULL);
    ASSERT(num == 1);
    ASSERT(gen == 0);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_DICT);
    TspdfObj *type_val = tspdf_dict_get(obj, "Type");
    ASSERT(type_val != NULL);
    ASSERT_EQ_STR((const char *)type_val->string.data, "Catalog");
    tspdf_arena_destroy(&a);
}

TEST(test_parse_stream) {
    const char *input = "1 0 obj\n<< /Length 5 >>\nstream\nHello\nendstream\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num;
    uint16_t gen;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STREAM);
    ASSERT(obj->stream.raw_len == 5);
    ASSERT(memcmp(p.data + obj->stream.raw_offset, "Hello", 5) == 0);
    tspdf_arena_destroy(&a);
}

// --- Xref tests ---

// Minimal classic xref PDF
static const char *MINI_PDF =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n"
    "xref\n"
    "0 4\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "trailer\n<< /TspdfSize 4 /Root 1 0 R >>\n"
    "startxref\n186\n%%EOF";

TEST(test_xref_parse_classic) {
    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 4);
    ASSERT(xref.entries[0].in_use == false); // obj 0 is free
    ASSERT(xref.entries[1].in_use == true);
    ASSERT(xref.entries[1].offset == 9);
    ASSERT(xref.trailer != NULL);
    TspdfObj *root = tspdf_dict_get(xref.trailer, "Root");
    ASSERT(root != NULL);
    ASSERT_EQ_INT(root->type, TSPDF_OBJ_REF);
    ASSERT(root->ref.num == 1);
    tspdf_arena_destroy(&a);
}

TEST(test_xref_resolve) {
    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    TspdfObj *obj1 = tspdf_xref_resolve(&xref, &p, 1, cache, NULL);
    ASSERT(obj1 != NULL);
    ASSERT_EQ_INT(obj1->type, TSPDF_OBJ_DICT);
    TspdfObj *type_val = tspdf_dict_get(obj1, "Type");
    ASSERT(type_val != NULL);
    ASSERT_EQ_STR((const char *)type_val->string.data, "Catalog");
    // Verify caching: same pointer returned
    TspdfObj *obj1b = tspdf_xref_resolve(&xref, &p, 1, cache, NULL);
    ASSERT(obj1 == obj1b);
    tspdf_arena_destroy(&a);
}

// --- Page tree / document open tests ---

TEST(test_document_open_mini) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)MINI_PDF, strlen(MINI_PDF), &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    // MediaBox [0 0 612 792]
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);
    ASSERT_EQ_INT(page->rotate, 0);
    tspdf_reader_destroy(doc);
}

TEST(test_document_open_invalid) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)"not a pdf", 9, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_document_page_inherit_rotate) {
    // PDF where /Rotate is on the /Pages node, inherited by child
    static const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /Rotate 90 /MediaBox [0 0 612 792] >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n"
        "xref\n"
        "0 4\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "0000000150 00000 n \n"
        "trailer\n<< /TspdfSize 4 /Root 1 0 R >>\n"
        "startxref\n197\n%%EOF";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 90);
    ASSERT(page->media_box[2] == 612.0);
    tspdf_reader_destroy(doc);
}

// --- Serialization tests ---

TEST(test_round_trip_save_reopen) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Save to memory
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out != NULL);
    ASSERT(out_len > 0);

    // Reopen the saved PDF
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    // Verify page attributes preserved
    for (size_t i = 0; i < 3; i++) {
        TspdfReaderPage *p1 = tspdf_reader_get_page(doc, i);
        TspdfReaderPage *p2 = tspdf_reader_get_page(doc2, i);
        ASSERT(p1->media_box[2] == p2->media_box[2]);
        ASSERT(p1->media_box[3] == p2->media_box[3]);
        ASSERT_EQ_INT(p1->rotate, p2->rotate);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_to_file) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_save(doc, "tests/data/round_trip.pdf");
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open_file("tests/data/round_trip.pdf", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    tspdf_reader_destroy(doc);
    remove("tests/data/round_trip.pdf");
}

// --- Manipulation tests ---

TEST(test_extract_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};  // extract first and third
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 2);

    // Round-trip to verify valid PDF
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 2);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

TEST(test_delete_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};  // delete second page
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(result), 2);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};
    TspdfReader *rotated = tspdf_reader_rotate(doc, pages, 2, 90, &err);
    ASSERT(rotated != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save and reopen to verify rotate persisted
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(rotated, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 0)->rotate, 90);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 1)->rotate, 0);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 2)->rotate, 90);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(doc);
}

TEST(test_reorder_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t order[] = {2, 0, 1};
    TspdfReader *reordered = tspdf_reader_reorder(doc, order, 3, &err);
    ASSERT(reordered != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reordered), 3);

    tspdf_reader_destroy(reordered);
    tspdf_reader_destroy(doc);
}

TEST(test_merge_documents) {
    TspdfError err;
    TspdfReader *doc1 = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc1 != NULL);
    TspdfReader *doc2 = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc2 != NULL);

    TspdfReader *docs[] = {doc1, doc2};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(merged), 4);

    // Round-trip
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 4);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc2);
    tspdf_reader_destroy(doc1);
}

TEST(test_extract_single_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 1);

    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

// --- Real PDF file tests ---

TEST(test_open_tspdf_one_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    // Default tspdf page size is A4: 595.276 x 841.89
    ASSERT(page->media_box[2] > 595.0 && page->media_box[2] < 596.0);
    ASSERT(page->media_box[3] > 841.0 && page->media_box[3] < 842.0);
    tspdf_reader_destroy(doc);
}

TEST(test_open_tspdf_three_pages) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 3);
    tspdf_reader_destroy(doc);
}

// --- Error handling tests ---

TEST(test_open_null_data) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(NULL, 0, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_open_truncated_pdf) {
    const char *truncated = "%PDF-1.4\n1 0 obj\n<<";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)truncated, strlen(truncated), &err);
    ASSERT(doc == NULL);
}

TEST(test_open_pdf_header_only_no_startxref_scan_crash) {
    static const char header_only[] = "%PDF-";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)header_only, strlen(header_only), &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);
}

TEST(test_open_pdf_rejects_overflowing_xref_subsection) {
    char pdf[256];
    int pdf_len = snprintf(pdf, sizeof(pdf),
                           "%%PDF-1.4\n"
                           "xref\n"
                           "%zu 1\n"
                           "0000000000 65535 f \n"
                           "trailer\n<< /Size 1 >>\n"
                           "startxref\n9\n%%EOF",
                           (size_t)-1);
    ASSERT(pdf_len > 0);
    ASSERT((size_t)pdf_len < sizeof(pdf));

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, (size_t)pdf_len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);
}

TEST(test_extract_out_of_bounds) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {5}; // out of bounds
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_invalid_angle) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_rotate(doc, pages, 1, 45, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
    tspdf_reader_destroy(doc);
}

TEST(test_merge_zero_docs) {
    TspdfError err;
    TspdfReader *result = tspdf_reader_merge(NULL, 0, &err);
    ASSERT(result == NULL);
    ASSERT(err != TSPDF_OK);
}

// NOTE: xref stream (PDF 1.5+) and object stream parsing are implemented
// but not tested here because hand-crafting binary xref streams is impractical.
// TODO: Add xref stream tests once real-world test PDFs are available.
// TODO: Add incremental update (/Prev chain) test once we have such a PDF.

TEST(test_open_encrypted_pdf_unsupported) {
    // Hand-crafted PDF with /Encrypt in trailer
    static const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n"
        "xref\n"
        "0 3\n"
        "0000000000 65535 f \n"
        "0000000009 00000 n \n"
        "0000000058 00000 n \n"
        "trailer\n<< /TspdfSize 3 /Root 1 0 R /Encrypt << /V 1 >> >>\n"
        "startxref\n110\n%%EOF";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_ENCRYPTED);
}

TEST(test_error_string) {
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_OK), "success");
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_PARSE), "PDF parsing failed");
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_UNSUPPORTED), "unsupported PDF feature");
}

TEST(test_encrypted_pdf_needs_password) {
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_ENCRYPTED),
                  "PDF is encrypted, password required");
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_BAD_PASSWORD),
                  "wrong password");
}

TEST(test_encrypt_aes128_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *encrypted_buf = NULL;
    size_t encrypted_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &encrypted_buf, &encrypted_len,
                                                  "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(encrypted_buf != NULL);

    /* Opening without password should fail */
    TspdfReader *doc2 = tspdf_reader_open(encrypted_buf, encrypted_len, &err);
    ASSERT(doc2 == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_ENCRYPTED);

    /* Wrong password should fail */
    doc2 = tspdf_reader_open_with_password(encrypted_buf, encrypted_len, "wrong", &err);
    ASSERT(doc2 == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_BAD_PASSWORD);

    /* Correct password should work */
    doc2 = tspdf_reader_open_with_password(encrypted_buf, encrypted_len, "user123", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    /* Save unencrypted (unlock) */
    uint8_t *unlocked_buf = NULL;
    size_t unlocked_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &unlocked_buf, &unlocked_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc3 = tspdf_reader_open(unlocked_buf, unlocked_len, &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 3);

    tspdf_reader_destroy(doc3);
    free(unlocked_buf);
    tspdf_reader_destroy(doc2);
    free(encrypted_buf);
    tspdf_reader_destroy(doc);
}

TEST(test_encrypt_aes256_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *encrypted_buf = NULL;
    size_t encrypted_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &encrypted_buf, &encrypted_len,
                                                  "pass", "owner", 0xFFFFFFFC, 256);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open_with_password(encrypted_buf, encrypted_len, "pass", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(encrypted_buf);
    tspdf_reader_destroy(doc);
}

TEST(test_encrypt_empty_user_password) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *encrypted_buf = NULL;
    size_t encrypted_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &encrypted_buf, &encrypted_len,
                                                  "", "owner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    /* Should auto-open (empty user password) */
    TspdfReader *doc2 = tspdf_reader_open(encrypted_buf, encrypted_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(encrypted_buf);
    tspdf_reader_destroy(doc);
}

TEST(test_manipulate_encrypted) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                  "test", "test", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open_with_password(enc, enc_len, "test", &err);
    ASSERT(doc2 != NULL);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc2, pages, 1, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc3 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 1);

    tspdf_reader_destroy(doc3);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc2);
    free(enc);
    tspdf_reader_destroy(doc);
}

// --- Metadata tests ---

TEST(test_metadata_set_and_read) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Test Title");
    tspdf_reader_set_author(doc, "Test Author");
    tspdf_reader_set_subject(doc, "Test Subject");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "Test Title");
    const char *author = tspdf_reader_get_author(doc2);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Test Author");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_remove_field) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Temp");
    tspdf_reader_set_title(doc, NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(tspdf_reader_get_title(doc2) == NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_preserved_through_manipulation) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "My Document");

    uint8_t *buf1 = NULL;
    size_t len1 = 0;
    err = tspdf_reader_save_to_memory(doc, &buf1, &len1);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(buf1, len1, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc2), "My Document");

    tspdf_reader_destroy(doc2);
    free(buf1);
    tspdf_reader_destroy(doc);
}

// --- Integration: full pipeline ---

TEST(test_full_pipeline) {
    TspdfError err;

    // Open 3-page doc
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Extract pages 0 and 2
    size_t extract[] = {0, 2};
    TspdfReader *extracted = tspdf_reader_extract(doc, extract, 2, &err);
    ASSERT(extracted != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(extracted), 2);

    // Rotate first page 90 degrees
    size_t rot_pages[] = {0};
    TspdfReader *rotated = tspdf_reader_rotate(extracted, rot_pages, 1, 90, &err);
    ASSERT(rotated != NULL);

    // Merge with original 1-page doc
    TspdfReader *one = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(one != NULL);
    TspdfReader *merge_docs[] = {rotated, one};
    TspdfReader *merged = tspdf_reader_merge(merge_docs, 2, &err);
    ASSERT(merged != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(merged), 3);

    // Save to memory, reopen, verify
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *final_doc = tspdf_reader_open(out, out_len, &err);
    ASSERT(final_doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(final_doc), 3);
    ASSERT_EQ_INT(tspdf_reader_get_page(final_doc, 0)->rotate, 90);

    tspdf_reader_destroy(final_doc);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(one);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
}

// --- Phase 3 integration test ---

TEST(test_phase3_full_pipeline) {
    TspdfError err;

    // Open 3-page PDF
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Overlay text on page 1
    TspdfWriter *res = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, font, 36.0);
    tspdf_stream_move_to(overlay, 72, 200);
    tspdf_stream_show_text(overlay, "WATERMARK");
    err = tspdf_page_end_content(doc, 0, overlay, res);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res);

    // Add annotations
    err = tspdf_page_add_link(doc, 0, 72, 700, 200, 20, "https://example.com");
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_page_add_stamp(doc, 1, 100, 400, 200, 50, "Draft");
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Set metadata
    tspdf_reader_set_title(doc, "Phase 3 Test");

    // Save encrypted
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                  "test", "test", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Reopen with password
    TspdfReader *doc2 = tspdf_reader_open_with_password(enc, enc_len, "test", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    // Save unencrypted
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Final reopen
    TspdfReader *doc3 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 3);

    tspdf_reader_destroy(doc3);
    free(out);
    tspdf_reader_destroy(doc2);
    free(enc);
    tspdf_reader_destroy(doc);
}

// --- Content overlay tests ---

TEST(test_overlay_text) {
    // Create a 1-page PDF
    TspdfWriter *creator = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(creator, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(creator);
    tspdf_stream_set_font(page, font, 24.0);
    tspdf_stream_move_to(page, 72, 400);
    tspdf_stream_show_text(page, "Original");
    uint8_t *pdf_data = NULL;
    size_t pdf_len = 0;
    tspdf_writer_save_to_memory(creator, &pdf_data, &pdf_len);
    tspdf_writer_destroy(creator);

    // Open with tspr
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf_data, pdf_len, &err);
    ASSERT(doc != NULL);

    // Overlay
    TspdfWriter *res_owner = tspdf_writer_create();
    const char *overlay_font = tspdf_writer_add_builtin_font(res_owner, "Courier");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, overlay_font, 18.0);
    tspdf_stream_move_to(overlay, 72, 300);
    tspdf_stream_show_text(overlay, "Overlay text");
    err = tspdf_page_end_content(doc, 0, overlay, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save and reopen
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
    free(pdf_data);
}

TEST(test_overlay_on_specific_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    TspdfStream *overlay = tspdf_page_begin_content(doc, 1);
    ASSERT(overlay != NULL);
    tspdf_stream_set_font(overlay, font, 36.0);
    tspdf_stream_move_to(overlay, 100, 100);
    tspdf_stream_show_text(overlay, "WATERMARK");
    err = tspdf_page_end_content(doc, 1, overlay, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
}

TEST(test_overlay_abort) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfStream *overlay = tspdf_page_begin_content(doc, 0);
    ASSERT(overlay != NULL);
    tspdf_page_abort_content(overlay);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out != NULL);

    free(out);
    tspdf_reader_destroy(doc);
}

// --- Annotation tests ---

TEST(test_add_link_annotation) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_link(doc, 0, 72, 700, 200, 20, "https://example.com");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfReaderPage *page = tspdf_reader_get_page(doc2, 0);
    TspdfObj *annots = tspdf_dict_get(page->page_dict, "Annots");
    ASSERT(annots != NULL);
    ASSERT_EQ_INT(annots->type, TSPDF_OBJ_ARRAY);
    ASSERT(annots->array.count >= 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_text_note) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_text_note(doc, 0, 100, 500, "Note", "This is a note");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_stamp) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_stamp(doc, 0, 100, 400, 200, 50, "Confidential");
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_add_link_to_page) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_page_add_link_to_page(doc, 0, 72, 700, 200, 20, 2);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

// --- Save options tests ---

TEST(test_save_options_default_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserve_object_ids) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_strip_unused) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_strip_metadata) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Should Be Removed");

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    // Title should be gone
    ASSERT(tspdf_reader_get_title(doc2) == NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_recompress) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

// --- Large document test (dynamic page growth) ---

TEST(test_large_document_500_pages) {
    TspdfWriter *doc = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(doc, "Helvetica");
    ASSERT(font != NULL);

    for (int i = 0; i < 500; i++) {
        TspdfStream *page = tspdf_writer_add_page(doc);
        ASSERT(page != NULL);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 12);
        tspdf_stream_move_to(page, 100, 700);
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d", i + 1);
        tspdf_stream_show_text(page, buf);
        tspdf_stream_end_text(page);
    }

    uint8_t *data = NULL;
    size_t len = 0;
    TspdfError terr = tspdf_writer_save_to_memory(doc, &data, &len);
    ASSERT(terr == TSPDF_OK);
    ASSERT(data != NULL);

    TspdfError err;
    TspdfReader *rdoc = tspdf_reader_open(data, len, &err);
    ASSERT(rdoc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(rdoc), 500);
    tspdf_reader_destroy(rdoc);

    free(data);
    tspdf_writer_destroy(doc);
}

int main(void) {
    printf("tspr reader tests:\n");

    printf("\n  Parser:\n");
    RUN(test_parse_integer);
    RUN(test_parse_negative_integer);
    RUN(test_parse_real);
    RUN(test_parse_bool_true);
    RUN(test_parse_null);
    RUN(test_parse_name);
    RUN(test_parse_name_hex_escape);
    RUN(test_parse_literal_string);
    RUN(test_parse_string_escape);
    RUN(test_parse_string_nested_parens);
    RUN(test_parse_hex_string);
    RUN(test_parse_array);
    RUN(test_parse_dict);
    RUN(test_parse_indirect_ref);
    RUN(test_parse_indirect_obj);
    RUN(test_parse_stream);

    printf("\n  Xref:\n");
    RUN(test_xref_parse_classic);
    RUN(test_xref_resolve);

    printf("\n  Document open:\n");
    RUN(test_document_open_mini);
    RUN(test_document_open_invalid);
    RUN(test_document_page_inherit_rotate);

    printf("\n  Serialization:\n");
    RUN(test_round_trip_save_reopen);
    RUN(test_save_to_file);

    printf("\n  Manipulation:\n");
    RUN(test_extract_pages);
    RUN(test_delete_pages);
    RUN(test_rotate_pages);
    RUN(test_reorder_pages);
    RUN(test_merge_documents);
    RUN(test_extract_single_page);

    printf("\n  Real PDF files:\n");
    RUN(test_open_tspdf_one_page);
    RUN(test_open_tspdf_three_pages);

    printf("\n  Error handling:\n");
    RUN(test_open_null_data);
    RUN(test_open_truncated_pdf);
    RUN(test_open_pdf_header_only_no_startxref_scan_crash);
    RUN(test_open_pdf_rejects_overflowing_xref_subsection);
    RUN(test_extract_out_of_bounds);
    RUN(test_rotate_invalid_angle);
    RUN(test_merge_zero_docs);
    RUN(test_open_encrypted_pdf_unsupported);
    RUN(test_error_string);

    printf("\n  Encryption:\n");
    RUN(test_encrypted_pdf_needs_password);
    RUN(test_encrypt_aes128_roundtrip);
    RUN(test_encrypt_aes256_roundtrip);
    RUN(test_encrypt_empty_user_password);
    RUN(test_manipulate_encrypted);

    printf("\n  Metadata:\n");
    RUN(test_metadata_set_and_read);
    RUN(test_metadata_remove_field);
    RUN(test_metadata_preserved_through_manipulation);

    printf("\n  Content overlay:\n");
    RUN(test_overlay_text);
    RUN(test_overlay_on_specific_page);
    RUN(test_overlay_abort);

    printf("\n  Annotations:\n");
    RUN(test_add_link_annotation);
    RUN(test_add_text_note);
    RUN(test_add_stamp);
    RUN(test_add_link_to_page);

    printf("\n  Integration:\n");
    RUN(test_full_pipeline);

    printf("\n  Phase 3 integration:\n");
    RUN(test_phase3_full_pipeline);

    printf("\n  Save options:\n");
    RUN(test_save_options_default_roundtrip);
    RUN(test_save_preserve_object_ids);
    RUN(test_save_strip_unused);
    RUN(test_save_strip_metadata);
    RUN(test_save_recompress);

    printf("\n  Large document:\n");
    RUN(test_large_document_500_pages);

    printf("\n%d tests, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
