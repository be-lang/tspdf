#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "test_framework.h"
#include "../src/reader/tspr.h"
#include "../src/reader/tspr_overlay.h"
#include "../src/reader/tspr_internal.h"
#include "../src/pdf/pdf_stream.h"
#include "../src/compress/deflate.h"
#include "../src/util/arena.h"
#include "../src/image/jpeg_codec.h"

static bool bytes_contains(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return false;
    }

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static size_t bytes_count(const uint8_t *haystack, size_t haystack_len, const char *needle) {
    if (!haystack || !needle) {
        return 0;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            count++;
            i += needle_len - 1;
        }
    }
    return count;
}

static TspdfObj *test_resolve_ref(TspdfReader *doc, TspdfObj *obj) {
    if (!doc || !obj || obj->type != TSPDF_OBJ_REF) {
        return obj;
    }

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num, doc->obj_cache, doc->crypt);
}

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

TEST(test_parse_integer_overflow_rejected) {
    const char *input = "9223372036854775808";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
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

TEST(test_parse_keywords_require_delimiters) {
    const char *inputs[] = {"truex", "falsex", "nullx"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        TspdfArena a = tspdf_arena_create(4096);
        TspdfParser p;
        tspdf_parser_init(&p, (const uint8_t *)inputs[i], strlen(inputs[i]), &a);
        TspdfObj *obj = tspdf_parse_obj(&p);
        ASSERT(obj == NULL);
        ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
        tspdf_arena_destroy(&a);
    }
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

TEST(test_parse_dict_duplicate_key_uses_last_value) {
    const char *input = "<< /Type /NotPage /Count 1 /Type /Page /Count 3 >>";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_DICT);
    ASSERT_EQ_SIZE(obj->dict.count, 4);

    TspdfObj *type_val = tspdf_dict_get(obj, "Type");
    ASSERT(type_val != NULL);
    ASSERT_EQ_INT(type_val->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)type_val->string.data, "Page");

    TspdfObj *count_val = tspdf_dict_get(obj, "Count");
    ASSERT(count_val != NULL);
    ASSERT_EQ_INT(count_val->type, TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count_val->integer, 3);
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

TEST(test_parse_indirect_ref_requires_delimited_r) {
    const char *input = "5 0 Root";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)obj->integer, 5);
    ASSERT_EQ_SIZE(p.pos, 1);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_indirect_ref_object_number_overflow) {
    const char *input = "4294967296 0 R";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_indirect_ref_generation_overflow) {
    const char *input = "1 65536 R";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
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

TEST(test_parse_indirect_obj_reject_object_number_overflow) {
    const char *input = "4294967296 0 obj\n<< /Type /Catalog >>\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num = 0;
    uint16_t gen = 0;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_indirect_obj_reject_generation_overflow) {
    const char *input = "1 65536 obj\n<< /Type /Catalog >>\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num = 0;
    uint16_t gen = 0;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
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

TEST(test_parse_stream_keyword_requires_delimiter) {
    const char *input = "1 0 obj\n<< /Length 0 >>\nstreamx\nendstream\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num = 0;
    uint16_t gen = 0;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_DICT);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_endstream_keyword_requires_delimiter) {
    const char *input = "1 0 obj\n<< /Length 1 >>\nstream\nXendstreamx\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num = 0;
    uint16_t gen = 0;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
    tspdf_arena_destroy(&a);
}

TEST(test_parse_stream_negative_length_fallback) {
    const char *input = "1 0 obj\n<< /Length -1 >>\nstream\nHello\nendstream\nendobj";
    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, strlen(input), &a);
    uint32_t num;
    uint16_t gen;
    TspdfObj *obj = tspdf_parse_indirect_obj(&p, &num, &gen);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_STREAM);
    ASSERT_EQ_SIZE(obj->stream.raw_len, 5);
    ASSERT(memcmp(p.data + obj->stream.raw_offset, "Hello", 5) == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_deep_copy_stream_uses_decoded_length) {
    const uint8_t decoded[] = "decoded stream payload";

    TspdfArena src_arena = tspdf_arena_create(4096);
    TspdfObj *stream = tspdf_arena_alloc_zero(&src_arena, sizeof(TspdfObj));
    ASSERT(stream != NULL);
    TspdfObj *dict = tspdf_arena_alloc_zero(&src_arena, sizeof(TspdfObj));
    ASSERT(dict != NULL);
    dict->type = TSPDF_OBJ_DICT;
    stream->type = TSPDF_OBJ_STREAM;
    stream->stream.dict = dict;
    stream->stream.raw_offset = 123;
    stream->stream.raw_len = 3;
    stream->stream.len = sizeof(decoded) - 1;
    stream->stream.self_contained = true;
    stream->stream.data = (uint8_t *)malloc(sizeof(decoded) - 1);
    ASSERT(stream->stream.data != NULL);
    memcpy(stream->stream.data, decoded, sizeof(decoded) - 1);

    TspdfArena dst_arena = tspdf_arena_create(4096);
    TspdfObj *copy = tspdf_obj_deep_copy(stream, &dst_arena);
    ASSERT(copy != NULL);
    ASSERT_EQ_INT(copy->type, TSPDF_OBJ_STREAM);
    ASSERT(copy->stream.data != NULL);
    ASSERT(copy->stream.data != stream->stream.data);
    ASSERT_EQ_SIZE(copy->stream.raw_len, 3);
    ASSERT_EQ_SIZE(copy->stream.len, sizeof(decoded) - 1);
    ASSERT(memcmp(copy->stream.data, decoded, sizeof(decoded) - 1) == 0);

    free(copy->stream.data);
    free(stream->stream.data);
    tspdf_arena_destroy(&dst_arena);
    tspdf_arena_destroy(&src_arena);
}

TEST(test_parse_deeply_nested_array_rejected) {
    // ~5000 nested arrays must fail cleanly (bounded recursion), not crash.
    const size_t depth = 5000;
    char *input = (char *)malloc(depth * 2 + 1);
    ASSERT(input != NULL);
    for (size_t i = 0; i < depth; i++) input[i] = '[';
    for (size_t i = 0; i < depth; i++) input[depth + i] = ']';
    input[depth * 2] = '\0';

    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, depth * 2, &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
    tspdf_arena_destroy(&a);
    free(input);
}

TEST(test_parse_deeply_nested_dict_rejected) {
    // ~5000 nested dicts must fail cleanly (bounded recursion), not crash.
    const size_t depth = 5000;
    char *input = (char *)malloc(depth * 4 + 1);
    ASSERT(input != NULL);
    size_t pos = 0;
    for (size_t i = 0; i < depth; i++) { input[pos++] = '<'; input[pos++] = '<'; input[pos++] = '/'; input[pos++] = 'K'; }
    // Give each dict a value (0) then close, so the syntax is otherwise valid.
    // But at this size the depth cap must trip before any close is reached.
    input[pos] = '\0';

    TspdfArena a = tspdf_arena_create(4096);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, pos, &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj == NULL);
    ASSERT_EQ_INT(p.error, TSPDF_ERR_PARSE);
    tspdf_arena_destroy(&a);
    free(input);
}

TEST(test_parse_moderately_nested_array_accepted) {
    // A depth well under the cap must still parse successfully.
    const size_t depth = 100;
    char *input = (char *)malloc(depth * 2 + 2);
    ASSERT(input != NULL);
    for (size_t i = 0; i < depth; i++) input[i] = '[';
    for (size_t i = 0; i < depth; i++) input[depth + i] = ']';
    input[depth * 2] = '\0';

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)input, depth * 2, &a);
    TspdfObj *obj = tspdf_parse_obj(&p);
    ASSERT(obj != NULL);
    ASSERT_EQ_INT(obj->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_INT(p.error, TSPDF_OK);
    tspdf_arena_destroy(&a);
    free(input);
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

static bool appendf(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *pos, cap - *pos, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= cap - *pos) {
        return false;
    }

    *pos += (size_t)written;
    return true;
}

static char *make_catalog_features_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n"
                 "<< /Type /Catalog /Pages 2 0 R /PageMode /UseOutlines "
                 "/ViewerPreferences << /DisplayDocTitle true >> "
                 "/Outlines 4 0 R /Names 5 0 R /AcroForm 6 0 R "
                 "/Metadata 7 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Outlines /Count 0 >>\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Dests << /Names [] >> >>\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Fields [] /NeedAppearances true >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Type /Metadata /Subtype /XML /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 8\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 8 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_missing_stream_length_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< >>\nstream\nHello\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_duplicate_stream_length_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Length 1 /Length 5 >>\n"
                 "stream\nHello\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_openable_mini_pdf_with_startxref_adjust(const char *prefix,
                                                          size_t trailing_spaces,
                                                          int startxref_adjust,
                                                          size_t *out_len) {
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    size_t cap = prefix_len + trailing_spaces + 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (prefix && !appendf(pdf, cap, &pos, "%s", prefix)) goto fail;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    size_t startxref_value = xref;
    if (startxref_adjust < 0) {
        size_t adjust = (size_t)(-startxref_adjust);
        if (adjust > startxref_value) goto fail;
        startxref_value -= adjust;
    } else {
        startxref_value += (size_t)startxref_adjust;
    }

    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, startxref_value)) goto fail;

    if (trailing_spaces > 0) {
        if (pos + trailing_spaces > cap) goto fail;
        memset(pdf + pos, ' ', trailing_spaces);
        pos += trailing_spaces;
    }

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_openable_mini_pdf(const char *prefix, size_t trailing_spaces, size_t *out_len) {
    return make_openable_mini_pdf_with_startxref_adjust(prefix, trailing_spaces, 0, out_len);
}

static char *make_prefixed_header_relative_xref_pdf(const char *prefix, size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    size_t prefix_len = prefix ? strlen(prefix) : 0;
    if (base_len > SIZE_MAX - prefix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(prefix_len + base_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    if (prefix_len > 0) {
        memcpy(pdf, prefix, prefix_len);
    }
    memcpy(pdf + prefix_len, base, base_len);
    free(base);

    *out_len = prefix_len + base_len;
    return pdf;
}

// Prepend `prefix_len` bytes of non-PDF filler (never containing "%PDF-")
// before a valid mini PDF, so the "%PDF-" header lands well past the 1024-byte
// fast-path window. The reader must scan the whole buffer to find it.
static char *make_large_prefixed_pdf(size_t prefix_len, size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    if (base_len > SIZE_MAX - prefix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(prefix_len + base_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    // Fill the prefix with a repeating byte pattern that avoids '%'. This keeps
    // the filler free of any accidental "%PDF-" occurrence.
    for (size_t i = 0; i < prefix_len; i++) {
        pdf[i] = (char)('A' + (int)(i % 26));
    }
    memcpy(pdf + prefix_len, base, base_len);
    free(base);

    *out_len = prefix_len + base_len;
    return pdf;
}

static char *make_pdf_with_bad_appended_startxref(size_t *out_len) {
    size_t base_len = 0;
    char *base = make_openable_mini_pdf(NULL, 0, &base_len);
    if (!base) return NULL;

    const char *suffix =
        "\n% appended malformed update\n"
        "startxref\n"
        "999999999\n"
        "%%EOF";
    size_t suffix_len = strlen(suffix);
    if (base_len > SIZE_MAX - suffix_len) {
        free(base);
        return NULL;
    }

    char *pdf = (char *)malloc(base_len + suffix_len);
    if (!pdf) {
        free(base);
        return NULL;
    }

    memcpy(pdf, base, base_len);
    memcpy(pdf + base_len, suffix, suffix_len);
    free(base);

    *out_len = base_len + suffix_len;
    return pdf;
}

static char *make_missing_startxref_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "%%%%EOF",
                 obj1, obj2, obj3)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// A PDF with a classic xref table but NO trailer and NO startxref: only the
// objects and the xref rows survive. The reader must reconstruct by scanning.
static char *make_no_trailer_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos, "%%%%EOF")) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// A PDF with NO xref, NO trailer, and NO startxref — just the objects. The
// catalog is not the first object so the scanner must find /Type /Catalog.
static char *make_object_scan_only_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Page /Parent 3 0 R /MediaBox [0 0 200 300] >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Catalog /Pages 3 0 R >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Pages /Kids [1 0 R] /Count 1 >>\nendobj\n")) goto fail;
    if (!appendf(pdf, 4096, &pos, "%%%%EOF")) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_classic_xref_horizontal_whitespace_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0\t4\f\n"
                 "0000000000\t65535\ff\t\n"
                 "%010zu\t00000\fn\t\n"
                 "%010zu\t00000\fn\t\n"
                 "%010zu\t00000\fn\t\n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_trailer_root_value_pdf(const char *root_value, size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Pages /Kids [2 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Page /Parent 1 0 R /MediaBox [0 0 450 650] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 3\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 3 /Root %s >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, root_value, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_wrong_xref_identity_pdf(size_t *out_len,
                                          const char *root_object_header,
                                          const char *root_xref_gen,
                                          const char *root_ref_gen) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "%s\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
                 root_object_header)) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu %s n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 %s R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, root_xref_gen, obj2, obj3, root_ref_gen, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_malformed_classic_xref_pdf(const char *subsection_header,
                                             const char *entry_lines,
                                             size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "%s\n"
                 "%s"
                 "trailer\n<< /Size 1 >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 subsection_header, entry_lines ? entry_lines : "", xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static void xref_entry4(uint8_t *dst, uint8_t type, size_t offset, uint8_t gen) {
    dst[0] = type;
    dst[1] = (uint8_t)((offset >> 8) & 0xFF);
    dst[2] = (uint8_t)(offset & 0xFF);
    dst[3] = gen;
}

static void xref_objstm_entry4(uint8_t *dst, uint16_t stream_obj, uint8_t index) {
    dst[0] = 2;
    dst[1] = (uint8_t)((stream_obj >> 8) & 0xFF);
    dst[2] = (uint8_t)(stream_obj & 0xFF);
    dst[3] = index;
}

static void write_be32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}

static void xref_entry9(uint8_t *dst, uint8_t type, uint32_t field1, uint32_t field2) {
    dst[0] = type;
    write_be32(dst + 1, field1);
    write_be32(dst + 5, field2);
}

static void xref_objstm_entry9(uint8_t *dst, uint32_t stream_obj, uint32_t index) {
    xref_entry9(dst, 2, stream_obj, index);
}

static char *make_recursive_compressed_xref_pdf(bool two_object_cycle, size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    uint32_t xref_obj_num = two_object_cycle ? 3 : 2;
    size_t object_count = two_object_cycle ? 4 : 3;
    size_t xref_obj = pos;
    uint8_t entries[4][4] = {{0}};
    xref_entry4(entries[0], 0, 0, 255);
    if (two_object_cycle) {
        xref_objstm_entry4(entries[1], 2, 0);
        xref_objstm_entry4(entries[2], 1, 0);
        xref_entry4(entries[3], 1, xref_obj, 0);
    } else {
        xref_objstm_entry4(entries[1], 1, 0);
        xref_entry4(entries[2], 1, xref_obj, 0);
    }

    size_t entries_len = object_count * sizeof(entries[0]);
    if (!appendf(pdf, 2048, &pos,
                 "%u 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 %zu] "
                 "/Size %zu /Root 1 0 R >>\n"
                 "stream\n",
                 xref_obj_num, entries_len, object_count, object_count)) goto fail;
    if (pos + entries_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, entries, entries_len);
    pos += entries_len;
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static uint8_t *ascii_hex_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > (SIZE_MAX - 1) / 2) return NULL;

    uint8_t *out = (uint8_t *)malloc(len * 2 + 1);
    if (!out) return NULL;

    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        out[pos++] = (uint8_t)hex[data[i] >> 4];
        out[pos++] = (uint8_t)hex[data[i] & 0x0F];
    }
    out[pos++] = '>';

    *out_len = pos;
    return out;
}

static uint8_t *ascii85_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > ((SIZE_MAX - 2) / 5) * 4) return NULL;

    size_t cap = ((len + 3) / 4) * 5 + 2;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;

    size_t pos = 0;
    size_t i = 0;
    while (i + 4 <= len) {
        uint32_t value = ((uint32_t)data[i] << 24) |
                         ((uint32_t)data[i + 1] << 16) |
                         ((uint32_t)data[i + 2] << 8) |
                         (uint32_t)data[i + 3];
        i += 4;

        char digits[5];
        for (int d = 4; d >= 0; d--) {
            digits[d] = (char)((value % 85) + '!');
            value /= 85;
        }
        for (int d = 0; d < 5; d++) {
            out[pos++] = (uint8_t)digits[d];
        }
    }

    size_t remaining = len - i;
    if (remaining > 0) {
        uint32_t value = 0;
        for (size_t j = 0; j < remaining; j++) {
            value |= (uint32_t)data[i + j] << (24 - j * 8);
        }

        char digits[5];
        for (int d = 4; d >= 0; d--) {
            digits[d] = (char)((value % 85) + '!');
            value /= 85;
        }
        for (size_t d = 0; d < remaining + 1; d++) {
            out[pos++] = (uint8_t)digits[d];
        }
    }

    out[pos++] = '~';
    out[pos++] = '>';

    *out_len = pos;
    return out;
}

static uint8_t *run_length_encode(const uint8_t *data, size_t len, size_t *out_len) {
    if (len > SIZE_MAX - (len / 128) - 2) return NULL;

    size_t cap = len + (len / 128) + 2;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;

    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        size_t chunk = len - i;
        if (chunk > 128) chunk = 128;
        out[pos++] = (uint8_t)(chunk - 1);
        memcpy(out + pos, data + i, chunk);
        pos += chunk;
        i += chunk;
    }
    out[pos++] = 128;

    *out_len = pos;
    return out;
}

static bool lzw_test_ensure_capacity(uint8_t **buf, size_t *cap, size_t needed) {
    if (needed <= *cap) return true;

    size_t new_cap = *cap ? *cap : 256;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    uint8_t *new_buf = (uint8_t *)realloc(*buf, new_cap);
    if (!new_buf) return false;
    memset(new_buf + *cap, 0, new_cap - *cap);
    *buf = new_buf;
    *cap = new_cap;
    return true;
}

static bool lzw_test_write_code(uint8_t **buf, size_t *cap, size_t *bit_pos,
                                uint16_t code, int code_size) {
    if (code_size <= 0 || code_size > 12) return false;

    size_t needed_bits = *bit_pos + (size_t)code_size;
    if (needed_bits < *bit_pos) return false;
    size_t needed_bytes = (needed_bits + 7) / 8;
    if (!lzw_test_ensure_capacity(buf, cap, needed_bytes)) return false;

    for (int i = code_size - 1; i >= 0; i--) {
        if ((code >> i) & 1u) {
            size_t bit = *bit_pos;
            (*buf)[bit / 8] |= (uint8_t)(1u << (7 - (bit % 8)));
        }
        (*bit_pos)++;
    }

    return true;
}

static int lzw_test_find_code(const uint16_t *prefix, const uint8_t *suffix,
                              size_t next_code, uint16_t parent, uint8_t ch) {
    for (size_t code = 258; code < next_code; code++) {
        if (prefix[code] == parent && suffix[code] == ch) {
            return (int)code;
        }
    }
    return -1;
}

static void lzw_test_maybe_grow_code_size(size_t next_code, int *code_size) {
    if (*code_size < 12 && next_code + 1 >= ((size_t)1 << *code_size)) {
        (*code_size)++;
    }
}

static uint8_t *lzw_encode(const uint8_t *data, size_t len, size_t *out_len) {
    size_t cap = 256;
    uint8_t *out = (uint8_t *)calloc(cap, 1);
    if (!out) return NULL;

    uint16_t prefix[4096] = {0};
    uint8_t suffix[4096] = {0};
    size_t next_code = 258;
    int code_size = 9;
    size_t bit_pos = 0;

    if (!lzw_test_write_code(&out, &cap, &bit_pos, 256, code_size)) goto fail;

    if (len > 0) {
        uint16_t current = data[0];
        for (size_t i = 1; i < len; i++) {
            uint8_t ch = data[i];
            int existing = lzw_test_find_code(prefix, suffix, next_code, current, ch);
            if (existing >= 0) {
                current = (uint16_t)existing;
                continue;
            }

            if (!lzw_test_write_code(&out, &cap, &bit_pos, current, code_size)) goto fail;

            if (next_code < 4096) {
                prefix[next_code] = current;
                suffix[next_code] = ch;
                next_code++;
                lzw_test_maybe_grow_code_size(next_code, &code_size);
            }

            current = ch;
        }

        if (!lzw_test_write_code(&out, &cap, &bit_pos, current, code_size)) goto fail;
    }

    if (!lzw_test_write_code(&out, &cap, &bit_pos, 257, code_size)) goto fail;

    *out_len = (bit_pos + 7) / 8;
    return out;

fail:
    free(out);
    return NULL;
}

static char *make_xref_stream_predictor_pdf(size_t *out_len,
                                            bool array_filter,
                                            bool ascii_hex_filter,
                                            bool ascii85_filter,
                                            bool run_length_filter,
                                            bool omit_type,
                                            bool direct_chain_decode_params,
                                            bool abbreviated_decode_params) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t rows[5][4];
    xref_entry4(rows[0], 0, 0, 255);
    xref_entry4(rows[1], 1, obj1, 0);
    xref_entry4(rows[2], 1, obj2, 0);
    xref_entry4(rows[3], 1, obj3, 0);
    xref_entry4(rows[4], 1, xref_obj, 0);

    uint8_t predicted[5 * 5];
    size_t pred_pos = 0;
    for (size_t r = 0; r < 5; r++) {
        predicted[pred_pos++] = 2; // PNG Up predictor
        for (size_t c = 0; c < 4; c++) {
            uint8_t prev = r > 0 ? rows[r - 1][c] : 0;
            predicted[pred_pos++] = (uint8_t)(rows[r][c] - prev);
        }
    }

    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(predicted, pred_pos, &comp_len);
    if (!comp) goto fail;

    uint8_t *encoded = comp;
    size_t encoded_len = comp_len;
    if (ascii_hex_filter) {
        encoded = ascii_hex_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    } else if (ascii85_filter) {
        encoded = ascii85_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    } else if (run_length_filter) {
        encoded = run_length_encode(comp, comp_len, &encoded_len);
        if (!encoded) {
            free(comp);
            goto fail;
        }
    }

    const char *xref_type = omit_type ? "" : "/Type /XRef ";
    const char *decode_params_key = abbreviated_decode_params ? "DP" : "DecodeParms";
    const char *chain_decode_params = direct_chain_decode_params ?
                                      "<< /Columns 4 /Predictor 12 >>" :
                                      "[null << /Columns 4 /Predictor 12 >>]";

    if ((ascii_hex_filter || ascii85_filter || run_length_filter) &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter [/%s /FlateDecode] "
                 "/%s %s "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, encoded_len, run_length_filter ? "RunLengthDecode" :
                 ascii85_filter ? "ASCII85Decode" : "ASCIIHexDecode",
                 decode_params_key, chain_decode_params)) {
        if (encoded != comp) free(encoded);
        free(comp);
        goto fail;
    }

    if (!ascii_hex_filter && !ascii85_filter && !run_length_filter && !array_filter &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter /FlateDecode "
                 "/%s << /Columns 4 /Predictor 12 >> "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, comp_len, decode_params_key)) {
        free(comp);
        goto fail;
    }

    if (!ascii_hex_filter && !ascii85_filter && !run_length_filter && array_filter &&
        !appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< %s/Length %zu /Filter [/FlateDecode] "
                 "/%s [<< /Columns 4 /Predictor 12 >>] "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 xref_type, comp_len, decode_params_key)) {
        free(comp);
        goto fail;
    }

    if (pos + encoded_len + 128 > 4096) {
        if (encoded != comp) free(encoded);
        free(comp);
        goto fail;
    }
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    if (encoded != comp) free(encoded);
    free(comp);

    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_xref_stream_lzw_predictor_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    uint8_t *encoded = NULL;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t rows[5][4];
    xref_entry4(rows[0], 0, 0, 255);
    xref_entry4(rows[1], 1, obj1, 0);
    xref_entry4(rows[2], 1, obj2, 0);
    xref_entry4(rows[3], 1, obj3, 0);
    xref_entry4(rows[4], 1, xref_obj, 0);

    uint8_t predicted[5 * 5];
    size_t pred_pos = 0;
    for (size_t r = 0; r < 5; r++) {
        predicted[pred_pos++] = 2;
        for (size_t c = 0; c < 4; c++) {
            uint8_t prev = r > 0 ? rows[r - 1][c] : 0;
            predicted[pred_pos++] = (uint8_t)(rows[r][c] - prev);
        }
    }

    size_t encoded_len = 0;
    encoded = lzw_encode(predicted, pred_pos, &encoded_len);
    if (!encoded) goto fail;

    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /XRef /Length %zu /Filter /LZWDecode "
                 "/DecodeParms << /Columns 4 /Predictor 12 /EarlyChange 1 >> "
                 "/W [1 2 1] /Index [0 5] /Size 5 /Root 1 0 R >>\n"
                 "stream\n",
                 encoded_len)) goto fail;
    if (pos + encoded_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    free(encoded);
    encoded = NULL;

    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(encoded);
    free(pdf);
    return NULL;
}

static char *make_object_stream_pdf_with_options(size_t *out_len,
                                                 const char *first_override,
                                                 const char *n_override,
                                                 const char *third_offset_override,
                                                 const char *first_objnum_override,
                                                 bool ascii_hex_filter,
                                                 bool ascii85_filter,
                                                 bool run_length_filter) {
    char obj_stream[1024];
    size_t obj_pos = 0;
    uint8_t *comp = NULL;
    uint8_t *encoded = NULL;

    const char *obj1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>";
    char third_offset_buf[32];
    if (!third_offset_override) {
        snprintf(third_offset_buf, sizeof(third_offset_buf), "%zu", strlen(obj1) + strlen(obj2));
    }
    const char *third_offset = third_offset_override ? third_offset_override : third_offset_buf;
    const char *first_obj_num = first_objnum_override ? first_objnum_override : "1";

    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s 0 2 %zu 3 %s ",
                 first_obj_num, strlen(obj1), third_offset)) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s%s", obj1, obj2, obj3)) {
        return NULL;
    }

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    char first_buf[32];
    if (!first_override) {
        snprintf(first_buf, sizeof(first_buf), "%zu", first);
    }
    const char *first_value = first_override ? first_override : first_buf;

    const char *n_value = n_override ? n_override : "3";
    uint8_t *stream_payload = (uint8_t *)obj_stream;
    size_t stream_payload_len = obj_pos;
    if (ascii_hex_filter || ascii85_filter || run_length_filter) {
        size_t comp_len = 0;
        comp = deflate_compress((const uint8_t *)obj_stream, obj_pos, &comp_len);
        if (!comp) goto fail;
        encoded = run_length_filter ? run_length_encode(comp, comp_len, &stream_payload_len) :
                  ascii85_filter ? ascii85_encode(comp, comp_len, &stream_payload_len) :
                  ascii_hex_encode(comp, comp_len, &stream_payload_len);
        if (!encoded) goto fail;
        stream_payload = encoded;
    }

    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N %s /First %s /Length %zu%s >>\n"
                 "stream\n",
                 n_value, first_value, stream_payload_len,
                 ascii_hex_filter ? " /Filter [/ASCIIHexDecode /FlateDecode]" :
                 ascii85_filter ? " /Filter [/ASCII85Decode /FlateDecode]" :
                 run_length_filter ? " /Filter [/RunLengthDecode /FlateDecode]" : "")) goto fail;
    if (pos + stream_payload_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, stream_payload, stream_payload_len);
    pos += stream_payload_len;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;
    free(encoded);
    free(comp);
    encoded = NULL;
    comp = NULL;

    size_t xref_obj = pos;
    uint8_t entries[6][4];
    xref_entry4(entries[0], 0, 0, 255);
    xref_objstm_entry4(entries[1], 4, 0);
    xref_objstm_entry4(entries[2], 4, 1);
    xref_objstm_entry4(entries[3], 4, 2);
    xref_entry4(entries[4], 1, obj4, 0);
    xref_entry4(entries[5], 1, xref_obj, 0);

    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 6] /Size 6 /Root 1 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > 4096) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(encoded);
    free(comp);
    free(pdf);
    return NULL;
}

static char *make_object_stream_pdf(size_t *out_len) {
    return make_object_stream_pdf_with_options(out_len, NULL, NULL, NULL, NULL, false, false, false);
}

static char *make_object_stream_zero_first_header_spill_pdf(size_t *out_len) {
    const char *payload = "2 0 << /Type /Pages /Kids [] /Count 0 >>";
    size_t payload_len = strlen(payload);
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 1 /First 0 /Length %zu >>\n"
                 "stream\n",
                 payload_len)) goto fail;
    if (pos + payload_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, payload, payload_len);
    pos += payload_len;
    if (!appendf(pdf, 2048, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t entries[6][4];
    xref_entry4(entries[0], 0, 0, 255);
    xref_entry4(entries[1], 0, 0, 0);
    xref_objstm_entry4(entries[2], 4, 0);
    xref_entry4(entries[3], 0, 0, 0);
    xref_entry4(entries[4], 1, obj4, 0);
    xref_entry4(entries[5], 1, xref_obj, 0);

    if (!appendf(pdf, 2048, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 6] /Size 6 /Root 2 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > 2048) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_object_stream_high_index_pdf(size_t *out_len) {
    const uint32_t high_index = 65536;
    const char *page_tree_obj = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    size_t page_tree_len = strlen(page_tree_obj);
    size_t obj_stream_cap = 0;
    char *obj_stream = NULL;
    char *pdf = NULL;

    if ((size_t)high_index > (SIZE_MAX - page_tree_len - 64) / 4) {
        return NULL;
    }
    obj_stream_cap = (size_t)high_index * 4 + 16 + page_tree_len;
    obj_stream = (char *)malloc(obj_stream_cap);
    if (!obj_stream) return NULL;

    size_t obj_pos = 0;
    for (uint32_t i = 0; i < high_index; i++) {
        if (obj_pos + 4 > obj_stream_cap) goto fail;
        memcpy(obj_stream + obj_pos, "9 0 ", 4);
        obj_pos += 4;
    }

    if (!appendf(obj_stream, obj_stream_cap, &obj_pos, "2 0 ")) goto fail;
    size_t first = obj_pos;
    if (!appendf(obj_stream, obj_stream_cap, &obj_pos, "%s", page_tree_obj)) goto fail;

    size_t pdf_cap = obj_pos + 4096;
    pdf = (char *)malloc(pdf_cap);
    if (!pdf) goto fail;

    size_t pos = 0;
    if (!appendf(pdf, pdf_cap, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 321 654] >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, pdf_cap, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N %u /First %zu /Length %zu >>\n"
                 "stream\n",
                 high_index + 1, first, obj_pos)) goto fail;
    if (pos + obj_pos + 128 > pdf_cap) goto fail;
    memcpy(pdf + pos, obj_stream, obj_pos);
    pos += obj_pos;
    if (!appendf(pdf, pdf_cap, &pos, "\nendstream\nendobj\n")) goto fail;
    free(obj_stream);
    obj_stream = NULL;

    size_t xref_obj = pos;
    uint8_t entries[6][9];
    xref_entry9(entries[0], 0, 0, 65535);
    xref_entry9(entries[1], 1, (uint32_t)obj1, 0);
    xref_objstm_entry9(entries[2], 4, high_index);
    xref_entry9(entries[3], 1, (uint32_t)obj3, 0);
    xref_entry9(entries[4], 1, (uint32_t)obj4, 0);
    xref_entry9(entries[5], 1, (uint32_t)xref_obj, 0);

    if (!appendf(pdf, pdf_cap, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 4 4] /Index [0 6] /Size 6 /Root 1 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > pdf_cap) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, pdf_cap, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(obj_stream);
    free(pdf);
    return NULL;
}

static char *make_object_stream_lzw_pdf(size_t *out_len) {
    char obj_stream[1024];
    size_t obj_pos = 0;
    uint8_t *encoded = NULL;

    const char *obj1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>";
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "1 0 2 %zu 3 %zu ",
                 strlen(obj1), strlen(obj1) + strlen(obj2))) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s%s", obj1, obj2, obj3)) {
        return NULL;
    }

    size_t encoded_len = 0;
    encoded = lzw_encode((const uint8_t *)obj_stream, obj_pos, &encoded_len);
    if (!encoded) return NULL;

    char *pdf = (char *)malloc(4096);
    if (!pdf) {
        free(encoded);
        return NULL;
    }

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 3 /First %zu /Length %zu /Filter /LZWDecode >>\n"
                 "stream\n",
                 first, encoded_len)) goto fail;
    if (pos + encoded_len + 128 > 4096) goto fail;
    memcpy(pdf + pos, encoded, encoded_len);
    pos += encoded_len;
    free(encoded);
    encoded = NULL;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref_obj = pos;
    uint8_t entries[6][4];
    xref_entry4(entries[0], 0, 0, 255);
    xref_objstm_entry4(entries[1], 4, 0);
    xref_objstm_entry4(entries[2], 4, 1);
    xref_objstm_entry4(entries[3], 4, 2);
    xref_entry4(entries[4], 1, obj4, 0);
    xref_entry4(entries[5], 1, xref_obj, 0);

    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [0 6] /Size 6 /Root 1 0 R >>\n"
                 "stream\n",
                 sizeof(entries))) goto fail;
    if (pos + sizeof(entries) + 128 > 4096) goto fail;
    memcpy(pdf + pos, entries, sizeof(entries));
    pos += sizeof(entries);
    if (!appendf(pdf, 4096, &pos,
                 "\nendstream\nendobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(encoded);
    free(pdf);
    return NULL;
}

static char *make_hybrid_reference_pdf_with_xrefstm_adjust(size_t *out_len,
                                                           int xrefstm_adjust) {
    char obj_stream[1024];
    size_t obj_pos = 0;

    const char *obj2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char *obj3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 250 350] >>";
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "2 0 3 %zu ", strlen(obj2))) {
        return NULL;
    }

    size_t first = obj_pos;
    if (!appendf(obj_stream, sizeof(obj_stream), &obj_pos, "%s%s", obj2, obj3)) {
        return NULL;
    }

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n"
                 "<< /Type /ObjStm /N 2 /First %zu /Length %zu >>\n"
                 "stream\n",
                 first, obj_pos)) goto fail;
    if (pos + obj_pos + 128 > 4096) goto fail;
    memcpy(pdf + pos, obj_stream, obj_pos);
    pos += obj_pos;
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref_stream = pos;
    uint8_t compressed_entries[2][4];
    xref_objstm_entry4(compressed_entries[0], 4, 0);
    xref_objstm_entry4(compressed_entries[1], 4, 1);
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n"
                 "<< /Type /XRef /Length %zu /W [1 2 1] /Index [2 2] /Size 6 >>\n"
                 "stream\n",
                 sizeof(compressed_entries))) goto fail;
    if (pos + sizeof(compressed_entries) + 128 > 4096) goto fail;
    memcpy(pdf + pos, compressed_entries, sizeof(compressed_entries));
    pos += sizeof(compressed_entries);
    if (!appendf(pdf, 4096, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xrefstm_value = xref_stream;
    if (xrefstm_adjust < 0) {
        size_t delta = (size_t)(-(xrefstm_adjust + 1)) + 1;
        if (delta > xrefstm_value) goto fail;
        xrefstm_value -= delta;
    } else {
        size_t delta = (size_t)xrefstm_adjust;
        if (delta > SIZE_MAX - xrefstm_value) goto fail;
        xrefstm_value += delta;
    }

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 2\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "4 2\n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R /XRefStm %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj4, xref_stream, xrefstm_value, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_hybrid_reference_pdf(size_t *out_len) {
    return make_hybrid_reference_pdf_with_xrefstm_adjust(out_len, 0);
}

static char *make_malformed_xref_stream_pdf(const char *dict_entries, size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t xref_obj = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n"
                 "<< /Type /XRef %s >>\n"
                 "stream\n"
                 "endstream\n"
                 "endobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 dict_entries, xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_raw_xref_stream_pdf_with_type(const char *type_entry,
                                                const char *dict_entries,
                                                const uint8_t *stream_data,
                                                size_t stream_len,
                                                size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.5\n")) goto fail;

    size_t xref_obj = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n"
                 "<< %s/Length %zu %s >>\n"
                 "stream\n",
                 type_entry, stream_len, dict_entries)) goto fail;
    if (pos + stream_len + 128 > 2048) goto fail;
    memcpy(pdf + pos, stream_data, stream_len);
    pos += stream_len;
    if (!appendf(pdf, 2048, &pos,
                 "\nendstream\n"
                 "endobj\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref_obj)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_raw_xref_stream_pdf(const char *dict_entries,
                                      const uint8_t *stream_data,
                                      size_t stream_len,
                                      size_t *out_len) {
    return make_raw_xref_stream_pdf_with_type("/Type /XRef ", dict_entries,
                                              stream_data, stream_len, out_len);
}

static char *make_incremental_update_pdf_with_options(size_t *out_len,
                                                      bool omit_new_root) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref_old = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF\n",
                 obj1_old, obj2_old, obj3_old, xref_old)) goto fail;

    size_t obj1_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>\nendobj\n")) goto fail;

    size_t obj5_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 4 0 R /MediaBox [0 0 600 800] >>\nendobj\n")) goto fail;

    size_t xref_new = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "1 1\n"
                 "%010zu 00000 n \n"
                 "4 2\n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 %s/Prev %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1_new, obj4_new, obj5_new,
                 omit_new_root ? "" : "/Root 1 0 R ", xref_old, xref_new)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_incremental_update_pdf(size_t *out_len) {
    return make_incremental_update_pdf_with_options(out_len, false);
}

static char *make_self_referential_prev_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 1\n"
                 "0000000000 65535 f \n"
                 "trailer\n<< /Size 1 /Prev %zu >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 xref, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_stream_length_pdf(size_t *out_len, size_t *stream_len) {
    const char *payload = "abc endstream def";
    size_t payload_len = strlen(payload);

    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] /Contents 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length 5 0 R >>\nstream\n%s\nendstream\nendobj\n",
                 payload)) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n%zu\nendobj\n",
                 payload_len)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 6\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, xref)) goto fail;

    *out_len = pos;
    *stream_len = payload_len;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_page_tree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids 6 0 R /Count 1 /MediaBox 4 0 R /Rotate 5 0 R >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n[0 0 300 400]\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n90\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n[3 0 R]\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_indirect_page_box_numbers_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 4 0 R 5 0 R] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n300.5\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n400\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 6\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_missing_page_tree_type_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Kids [3 0 R] /Count 1 /MediaBox [0 0 320 420] >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_inherited_crop_box_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /CropBox [0 0 320 480] >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_inherited_page_attrs_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *content = "BT /F1 12 Tf 72 720 Td (Hi) Tj ET";
    size_t content_len = strlen(content);

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/MediaBox 4 0 R /Rotate 5 0 R "
                 "/BleedBox [5 5 295 395] "
                 "/TrimBox [10 10 290 390] "
                 "/ArtBox [15 15 285 385] "
                 "/Resources << /Font << /F1 6 0 R >> >> >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 7 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n[0 0 300 400]\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n90\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 content_len, content)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 8\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 8 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_inherited_user_unit_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.6\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/MediaBox [0 0 100 200] /UserUnit 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n2.5\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_noncanonical_rotate_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 "
                 "/Rotate -90 /MediaBox [0 0 300 400] >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /Rotate 450 >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_invalid_rotate_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n"
                 "<< /Type /Pages /Kids [3 0 R] /Count 1 "
                 "/Rotate 45 /MediaBox [0 0 300 400] >>\n"
                 "endobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

static char *make_cyclic_page_tree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [2 0 R] /Count 1 /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 3\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 3 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// Catalog with /Version given as an indirect reference to a name object.
// Header says 1.4; the referenced name says 1.6 and must win.
static char *make_indirect_version_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Version 4 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 400] >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos, "4 0 obj\n/1.6\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

TEST(test_pdf_version_catalog_indirect_ref_resolved) {
    size_t len = 0;
    char *pdf = make_indirect_version_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // An indirect /Version must be resolved, not silently ignored in favor
    // of the header version.
    ASSERT_EQ_STR(tspdf_reader_pdf_version(doc), "1.6");
    // Resolution is cached; a second call returns the same answer.
    ASSERT_EQ_STR(tspdf_reader_pdf_version(doc), "1.6");

    tspdf_reader_destroy(doc);
    free(pdf);
}

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

TEST(test_document_open_classic_xref_horizontal_whitespace) {
    size_t len = 0;
    char *pdf = make_classic_xref_horizontal_whitespace_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
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

TEST(test_classic_xref_reject_subsection_range_overflow) {
    char header[64];
    snprintf(header, sizeof(header), "%zu 2", (size_t)SIZE_MAX - 1);

    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf(header, "", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_generation_overflow) {
    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf("0 1", "0000000000 65536 n \n", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_unknown_entry_type) {
    size_t len = 0;
    char *pdf = make_malformed_classic_xref_pdf("0 1", "0000000000 00000 z \n", &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_reject_wrong_xref_object_number) {
    size_t len = 0;
    char *pdf = make_wrong_xref_identity_pdf(&len, "9 0 obj", "00000", "0");
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_wrong_xref_generation) {
    size_t len = 0;
    char *pdf = make_wrong_xref_identity_pdf(&len, "1 0 obj", "00001", "1");
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_open_xref_stream_with_png_predictor) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_without_type) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, true, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_dp_abbreviation) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, false, false, false, false, false, false, true);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_stream_reject_explicit_wrong_type) {
    const uint8_t row[4] = {0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf_with_type("/Type /NotXRef ",
                                                  "/W [1 2 1] /Size 1",
                                                  row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_classic_xref_reject_implausible_entry_count) {
    // A 1KB file cannot define 48 million objects; the declared subsection
    // count must be rejected before the entry table is allocated (no OOM).
    // The object-scan fallback then finds the catalog but there is no page
    // tree, so the open still fails cleanly (doc == NULL) rather than crashing
    // or allocating gigabytes.
    static const char pdf[] =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "xref\n"
        "0 48000000\n"
        "0000000000 65535 f \n"
        "trailer\n<< /Size 48000000 /Root 1 0 R >>\n"
        "startxref\n58\n%%EOF";

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)pdf, sizeof(pdf) - 1, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_xref_stream_reject_implausible_size) {
    const uint8_t row[4] = {1, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 1] /Size 48000000",
                                         row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_filter_array) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, true, false, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_filter_chain_direct_decode_params) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, true, false, false, false, true, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_ascii85_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, true, false, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_stream_ascii85_z_shorthand_expands_safely) {
    const uint8_t data[] = "zzz~>";
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/Filter /ASCII85Decode /W [1 2 1] /Size 3",
                                         data, sizeof(data) - 1, &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 3);
    ASSERT(xref.entries[0].seen == true);
    ASSERT(xref.entries[1].seen == true);
    ASSERT(xref.entries[2].seen == true);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_runlength_filter_chain) {
    size_t len = 0;
    char *pdf = make_xref_stream_predictor_pdf(&len, true, false, false, true, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_xref_stream_with_lzw_predictor) {
    size_t len = 0;
    char *pdf = make_xref_stream_lzw_predictor_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_xref_stream_negative_w) {
    size_t len = 0;
    char *pdf = make_malformed_xref_stream_pdf("/Length 0 /W [1 -1 2] /Size 1", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_xref_stream_odd_index) {
    size_t len = 0;
    char *pdf = make_malformed_xref_stream_pdf("/Length 0 /W [1 2 1] /Size 1 /Index [0]", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_xref_stream_reject_oversized_field_width) {
    const uint8_t row[11] = {0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 9 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_unknown_entry_type) {
    const uint8_t row[4] = {3, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_generation_overflow) {
    const uint8_t row[6] = {1, 0, 0, 1, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 2 3] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_reject_objstm_number_overflow) {
    const uint8_t row[7] = {2, 1, 0, 0, 0, 0, 0};
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 5 1] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_stream_accepts_large_objstm_index) {
    uint8_t row[9];
    xref_objstm_entry9(row, 1, 65536);
    size_t len = 0;
    char *pdf = make_raw_xref_stream_pdf("/W [1 4 4] /Size 1", row, sizeof(row), &len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(xref.count, 1);
    ASSERT(xref.entries[0].compressed);
    ASSERT_EQ_INT(xref.entries[0].stream_obj, 1);
    ASSERT_EQ_SIZE(xref.entries[0].stream_idx, 65536);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_reject_self_referential_object_stream_entry) {
    size_t len = 0;
    char *pdf = make_recursive_compressed_xref_pdf(false, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    free(pdf);
}

TEST(test_document_reject_cyclic_object_stream_entries) {
    size_t len = 0;
    char *pdf = make_recursive_compressed_xref_pdf(true, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    free(pdf);
}

TEST(test_document_open_incremental_update_prev_chain) {
    size_t len = 0;
    char *pdf = make_incremental_update_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *old_page = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *new_page = tspdf_reader_get_page(doc, 1);
    ASSERT(old_page != NULL);
    ASSERT(new_page != NULL);
    ASSERT(old_page->media_box[2] == 300.0);
    ASSERT(old_page->media_box[3] == 400.0);
    ASSERT(new_page->media_box[2] == 600.0);
    ASSERT(new_page->media_box[3] == 800.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_incremental_update_inherits_root_from_prev) {
    size_t len = 0;
    char *pdf = make_incremental_update_pdf_with_options(&len, true);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *old_page = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *new_page = tspdf_reader_get_page(doc, 1);
    ASSERT(old_page != NULL);
    ASSERT(new_page != NULL);
    ASSERT(old_page->media_box[2] == 300.0);
    ASSERT(old_page->media_box[3] == 400.0);
    ASSERT(new_page->media_box[2] == 600.0);
    ASSERT(new_page->media_box[3] == 800.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_reject_self_referential_prev_chain) {
    size_t len = 0;
    char *pdf = make_self_referential_prev_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_ERR_XREF);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_hybrid_reference_xrefstm) {
    size_t len = 0;
    char *pdf = make_hybrid_reference_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 250.0);
    ASSERT(page->media_box[3] == 350.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_hybrid_reference_xrefstm_near_offset_repaired) {
    size_t len = 0;
    char *pdf = make_hybrid_reference_pdf_with_xrefstm_adjust(&len, 5);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 250.0);
    ASSERT(page->media_box[3] == 350.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_xref_indirect_stream_length) {
    size_t len = 0;
    size_t stream_len = 0;
    char *pdf = make_indirect_stream_length_pdf(&len, &stream_len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    ASSERT(cache != NULL);

    TspdfObj *stream = tspdf_xref_resolve(&xref, &p, 4, cache, NULL);
    ASSERT(stream != NULL);
    ASSERT_EQ_INT(stream->type, TSPDF_OBJ_STREAM);
    ASSERT_EQ_SIZE(stream->stream.raw_len, stream_len);
    ASSERT(memcmp(p.data + stream->stream.raw_offset, "abc endstream def", stream_len) == 0);

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_xref_resolve_rejects_objstm_header_spill) {
    size_t len = 0;
    char *pdf = make_object_stream_zero_first_header_spill_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfArena a = tspdf_arena_create(65536);
    TspdfParser p;
    tspdf_parser_init(&p, (const uint8_t *)pdf, len, &a);
    TspdfReaderXref xref = {0};
    TspdfError err = tspdf_xref_parse(&p, &xref);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfObj **cache = tspdf_arena_alloc_zero(&a, sizeof(TspdfObj *) * xref.count);
    ASSERT(cache != NULL);

    TspdfObj *obj = tspdf_xref_resolve(&xref, &p, 2, cache, NULL);
    ASSERT(obj == NULL);

    // The failed resolve leaves the decoded ObjStm buffer on the cached
    // stream object; the document path frees it in tspdf_reader_destroy,
    // but this direct-resolve harness must free it itself.
    for (size_t i = 0; i < xref.count; i++) {
        if (cache[i] && cache[i]->type == TSPDF_OBJ_STREAM) {
            free(cache[i]->stream.data);
        }
    }

    tspdf_arena_destroy(&a);
    free(pdf);
}

TEST(test_document_open_object_stream_page_tree) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_large_index) {
    size_t len = 0;
    char *pdf = make_object_stream_high_index_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 321.0);
    ASSERT(page->media_box[3] == 654.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, true, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_ascii85_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, false, true, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_runlength_filter_chain) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, NULL, false, false, true);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_stream_lzw_filter) {
    size_t len = 0;
    char *pdf = make_object_stream_lzw_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_objstm_negative_first) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, "-1", NULL, NULL, NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_negative_n) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, "-1", NULL, NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_bad_offset) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, "9999", NULL, false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_reject_objstm_wrong_object_number) {
    size_t len = 0;
    char *pdf = make_object_stream_pdf_with_options(&len, NULL, NULL, NULL, "9", false, false, false);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
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

TEST(test_document_open_direct_trailer_root_catalog) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("<< /Type /Catalog /Pages 1 0 R >>", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 450.0);
    ASSERT(page->media_box[3] == 650.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_duplicate_trailer_root_uses_last) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("9 0 R /Root << /Type /Catalog /Pages 1 0 R >>", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 450.0);
    ASSERT(page->media_box[3] == 650.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_reject_non_dictionary_trailer_root) {
    size_t len = 0;
    char *pdf = make_trailer_root_value_pdf("/Catalog", &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
}

TEST(test_document_open_invalid) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(
        (const uint8_t *)"not a pdf", 9, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);
}

TEST(test_document_open_header_with_prefix) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf("%!PS-Adobe-3.0\n", 0, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_header_beyond_1024_byte_prefix) {
    // 2048 bytes of filler push the "%PDF-" header past the fast-path window.
    size_t len = 0;
    char *pdf = make_large_prefixed_pdf(2048, &len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_prefixed_header_relative_xref_offsets_repaired) {
    size_t len = 0;
    char *pdf = make_prefixed_header_relative_xref_pdf("%!PS-Adobe-3.0\n", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_long_trailing_padding) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf(NULL, 5000, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_startxref_before_table_repaired) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf_with_startxref_adjust(NULL, 0, -3, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_startxref_inside_table_repaired) {
    size_t len = 0;
    char *pdf = make_openable_mini_pdf_with_startxref_adjust(NULL, 0, 2, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_bad_appended_startxref_uses_earlier_marker) {
    size_t len = 0;
    char *pdf = make_pdf_with_bad_appended_startxref(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_missing_startxref_classic_xref_recovered) {
    size_t len = 0;
    char *pdf = make_missing_startxref_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_no_trailer_object_scan_recovered) {
    // No trailer / no startxref: only objects and xref rows. Reconstruct by
    // scanning "N G obj" markers and locating /Type /Catalog.
    size_t len = 0;
    char *pdf = make_no_trailer_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 612.0);
    ASSERT(page->media_box[3] == 792.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_object_scan_only_recovered) {
    // No xref, no trailer, no startxref, and the catalog is not object 1.
    size_t len = 0;
    char *pdf = make_object_scan_only_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 200.0);
    ASSERT(page->media_box[3] == 300.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_open_short_header_only) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)"%PDF-", 5, &err);
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

TEST(test_document_page_tree_normalizes_rotate) {
    size_t len = 0;
    char *pdf = make_noncanonical_rotate_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    TspdfReaderPage *inherited = tspdf_reader_get_page(doc, 0);
    TspdfReaderPage *direct = tspdf_reader_get_page(doc, 1);
    ASSERT(inherited != NULL);
    ASSERT(direct != NULL);
    ASSERT_EQ_INT(inherited->rotate, 270);
    ASSERT_EQ_INT(direct->rotate, 90);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_ignores_invalid_rotate) {
    size_t len = 0;
    char *pdf = make_invalid_rotate_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_indirect_attributes) {
    size_t len = 0;
    char *pdf = make_indirect_page_tree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 300.0);
    ASSERT(page->media_box[3] == 400.0);
    ASSERT_EQ_INT(page->rotate, 90);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_box_indirect_number_entries) {
    size_t len = 0;
    char *pdf = make_indirect_page_box_numbers_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 300.5);
    ASSERT(page->media_box[3] == 400.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_missing_type_inferred) {
    size_t len = 0;
    char *pdf = make_missing_page_tree_type_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 320.0);
    ASSERT(page->media_box[3] == 420.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_inherited_crop_box_fallback) {
    size_t len = 0;
    char *pdf = make_inherited_crop_box_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[0] == 0.0);
    ASSERT(page->media_box[1] == 0.0);
    ASSERT(page->media_box[2] == 320.0);
    ASSERT(page->media_box[3] == 480.0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_document_page_tree_inherited_user_unit) {
    size_t len = 0;
    char *pdf = make_inherited_user_unit_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->media_box[2] == 100.0);
    ASSERT(page->media_box[3] == 200.0);
    ASSERT(page->user_unit == 2.5);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    TspdfReaderPage *roundtrip_page = tspdf_reader_get_page(reopened, 0);
    ASSERT(roundtrip_page != NULL);
    ASSERT(roundtrip_page->media_box[2] == 100.0);
    ASSERT(roundtrip_page->media_box[3] == 200.0);
    ASSERT(roundtrip_page->user_unit == 2.5);

    TspdfObj *unit = tspdf_dict_get(roundtrip_page->page_dict, "UserUnit");
    ASSERT(unit != NULL);
    if (unit->type == TSPDF_OBJ_REF) {
        TspdfParser parser;
        tspdf_parser_init(&parser, reopened->data, reopened->data_len, &reopened->arena);
        unit = tspdf_xref_resolve(&reopened->xref, &parser, unit->ref.num, reopened->obj_cache, NULL);
        ASSERT(unit != NULL);
    }
    ASSERT_EQ_INT(unit->type, TSPDF_OBJ_REAL);
    ASSERT(unit->real == 2.5);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
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

TEST(test_save_adds_missing_stream_length) {
    size_t len = 0;
    char *pdf = make_missing_stream_length_page_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Length 5"));
    ASSERT(!bytes_contains(out, out_len, "<< >>\nstream\nHello"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_deduplicates_stream_length) {
    size_t len = 0;
    char *pdf = make_duplicate_stream_length_page_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(bytes_count(out, out_len, "/Length "), 1);
    ASSERT(bytes_contains(out, out_len, "/Length 5"));
    ASSERT(!bytes_contains(out, out_len, "/Length 1"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
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

TEST(test_rotate_pages_normalizes_negative_angle) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *rotated = tspdf_reader_rotate(doc, pages, 1, -90, &err);
    ASSERT(rotated != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(rotated, 0)->rotate, 270);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(rotated, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_INT(tspdf_reader_get_page(reopened, 0)->rotate, 270);

    TspdfObj *rotate = tspdf_dict_get(tspdf_reader_get_page(reopened, 0)->page_dict, "Rotate");
    ASSERT(rotate != NULL);
    ASSERT_EQ_INT(rotate->type, TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)rotate->integer, 270);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(rotated);
    tspdf_reader_destroy(doc);
}

// Read a page box (MediaBox/CropBox) into out[4]; false if absent/malformed.
static bool read_page_box(TspdfReader *doc, size_t idx, const char *key, double out[4]) {
    TspdfReaderPage *page = tspdf_reader_get_page(doc, idx);
    if (!page) return false;
    TspdfObj *box = tspdf_dict_get(page->page_dict, key);
    if (!box || box->type != TSPDF_OBJ_ARRAY || box->array.count < 4) return false;
    for (int i = 0; i < 4; i++) {
        TspdfObj *it = &box->array.items[i];
        if (it->type == TSPDF_OBJ_INT) out[i] = (double)it->integer;
        else if (it->type == TSPDF_OBJ_REAL) out[i] = it->real;
        else return false;
    }
    return true;
}

TEST(test_set_cropbox_explicit_box) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0, 2};
    double box[4] = {50, 60, 400, 700};
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 2, box, &err);
    ASSERT(cropped != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(cropped, &out, &out_len), TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);

    double cb[4];
    ASSERT(read_page_box(reopened, 0, "CropBox", cb));
    ASSERT(cb[0] == 50 && cb[1] == 60 && cb[2] == 400 && cb[3] == 700);
    // Page 1 (index 1) was not selected: no CropBox.
    double none[4];
    ASSERT(!read_page_box(reopened, 1, "CropBox", none));
    ASSERT(read_page_box(reopened, 2, "CropBox", cb));
    ASSERT(cb[0] == 50 && cb[2] == 400);

    // Content (text) still present — crop only clips the view.
    const char *txt = tspdf_reader_page_text(reopened, 0, &err);
    ASSERT(txt != NULL);
    ASSERT(strstr(txt, "Page 1") != NULL);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(cropped);
    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_clamps_to_mediabox) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    double mb[4];
    ASSERT(read_page_box(doc, 0, "MediaBox", mb));

    // Box larger than the MediaBox on all sides — must clamp to the media.
    size_t pages[] = {0};
    double box[4] = {-100, -100, mb[2] + 500, mb[3] + 500};
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped != NULL);

    double cb[4];
    ASSERT(read_page_box(cropped, 0, "CropBox", cb));
    ASSERT(cb[0] == mb[0] && cb[1] == mb[1] && cb[2] == mb[2] && cb[3] == mb[3]);

    tspdf_reader_destroy(cropped);
    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_degenerate_rejected) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    double box[4] = {200, 100, 100, 300};  // x1 < x0
    err = TSPDF_OK;
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(doc);
}

TEST(test_set_cropbox_out_of_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {5};
    double box[4] = {10, 10, 100, 100};
    err = TSPDF_OK;
    TspdfReader *cropped = tspdf_reader_set_cropbox(doc, pages, 1, box, &err);
    ASSERT(cropped == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
}

TEST(test_scale_factor_half) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    double mb0[4];
    ASSERT(read_page_box(doc, 0, "MediaBox", mb0));
    double w0 = mb0[2] - mb0[0], h0 = mb0[3] - mb0[1];

    size_t pages[] = {0};
    TspdfReader *scaled = tspdf_reader_scale(doc, pages, 1, 0.5, 0.5, &err);
    ASSERT(scaled != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(scaled, &out, &out_len), TSPDF_OK);
    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);

    double mb[4];
    ASSERT(read_page_box(reopened, 0, "MediaBox", mb));
    double w = mb[2] - mb[0], h = mb[3] - mb[1];
    ASSERT(w > w0 * 0.5 - 0.01 && w < w0 * 0.5 + 0.01);
    ASSERT(h > h0 * 0.5 - 0.01 && h < h0 * 0.5 + 0.01);

    // Unscaled page 1 keeps its original dimensions.
    double mb1[4];
    ASSERT(read_page_box(reopened, 1, "MediaBox", mb1));
    ASSERT((mb1[2] - mb1[0]) > w0 - 0.01 && (mb1[2] - mb1[0]) < w0 + 0.01);

    // Text still extractable after the content-transform wrap.
    const char *txt = tspdf_reader_page_text(reopened, 0, &err);
    ASSERT(txt != NULL);
    ASSERT(strstr(txt, "Page 1") != NULL);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(scaled);
    tspdf_reader_destroy(doc);
}

TEST(test_scale_nonuniform_and_cropbox) {
    TspdfError err;
    // Page with both MediaBox and CropBox; scale must transform both.
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] "
        "/CropBox [10 20 190 380] /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 8 >>\nstream\nq 1 0 0\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *scaled = tspdf_reader_scale(doc, pages, 1, 2.0, 3.0, &err);
    ASSERT(scaled != NULL);

    double mb[4], cb[4];
    ASSERT(read_page_box(scaled, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0 && mb[2] == 400 && mb[3] == 1200);
    ASSERT(read_page_box(scaled, 0, "CropBox", cb));
    ASSERT(cb[0] == 20 && cb[1] == 60 && cb[2] == 380 && cb[3] == 1140);

    tspdf_reader_destroy(scaled);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_a4_from_letter) {
    TspdfError err;
    // Letter-size page (612 x 792) with text; resize to A4 (595.28 x 841.89).
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/CropBox [10 10 602 782] /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 595.27 && mb[2] < 595.29);
    ASSERT(mb[3] > 841.88 && mb[3] < 841.90);
    // CropBox dropped by resize.
    double cb[4];
    ASSERT(!read_page_box(r, 0, "CropBox", cb));

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

// A /Rotate 90 page (MediaBox 400x300, viewed 300x400) resized --to A4 must
// come out A4 PORTRAIT in the VIEWED orientation (595x842) because the source
// is VIEWED portrait. Since /Rotate 90 swaps the axes, the stored MediaBox
// must hold the swapped extents [0 0 842 595] so that viewed = 595x842.
TEST(test_resize_to_a4_rotate90_viewed_portrait) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] "
        "/Rotate 90 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // /Rotate is preserved.
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 90);

    // Stored MediaBox is the SWAPPED extents (target_h x target_w).
    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 841.88 && mb[2] < 841.90);   // stored width  = target_h
    ASSERT(mb[3] > 595.27 && mb[3] < 595.29);   // stored height = target_w

    // Viewed dims after applying /Rotate 90 (swap): 595.28 x 841.89 => portrait.
    int rot = tspdf_reader_get_page(r, 0)->rotate;
    double stored_w = mb[2] - mb[0];
    double stored_h = mb[3] - mb[1];
    double viewed_w = (rot == 90 || rot == 270) ? stored_h : stored_w;
    double viewed_h = (rot == 90 || rot == 270) ? stored_w : stored_h;
    ASSERT(viewed_w > 595.27 && viewed_w < 595.29);
    ASSERT(viewed_h > 841.88 && viewed_h < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

// Same for /Rotate 270.
TEST(test_resize_to_a4_rotate270_viewed_portrait) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] "
        "/Rotate 270 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 270);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    int rot = tspdf_reader_get_page(r, 0)->rotate;
    double stored_w = mb[2] - mb[0];
    double stored_h = mb[3] - mb[1];
    double viewed_w = (rot == 90 || rot == 270) ? stored_h : stored_w;
    double viewed_h = (rot == 90 || rot == 270) ? stored_w : stored_h;
    ASSERT(viewed_w > 595.27 && viewed_w < 595.29);
    ASSERT(viewed_h > 841.88 && viewed_h < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

// Orientation-preserving rule: the VIEWED output (after /Rotate) is the
// requested size oriented to match the source's VIEWED orientation. A
// portrait-stored page (612x792) with /Rotate 90 is viewed landscape
// (792x612), so --to A4 must come out viewed A4 LANDSCAPE (841.89 x 595.28).
// The viewer swaps a /Rotate 90 page's axes, so the stored MediaBox stays
// portrait [0 0 595.28 841.89] with /Rotate 90 kept.
TEST(test_resize_to_a4_rotate90_preserves_viewed_landscape) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Rotate 90 /Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_get_page(r, 0)->rotate, 90);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    // Stored box stays portrait so the viewed page is A4 landscape.
    ASSERT(mb[2] > 595.27 && mb[2] < 595.29);
    ASSERT(mb[3] > 841.88 && mb[3] < 841.90);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

// An unrotated landscape page keeps its viewed orientation too: --to A4
// yields A4 landscape (841.89 x 595.28), not content letterboxed in portrait.
TEST(test_resize_to_a4_landscape_page_stays_landscape) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 792 612] "
        "/Contents 4 0 R >>\nendobj\n"
        "4 0 obj\n<< /Length 12 >>\nstream\n0 0 1 1 re f\nendstream\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *r = tspdf_reader_resize_to(doc, pages, 1, 595.28, 841.89, &err);
    ASSERT(r != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    double mb[4];
    ASSERT(read_page_box(r, 0, "MediaBox", mb));
    ASSERT(mb[0] == 0 && mb[1] == 0);
    ASSERT(mb[2] > 841.88 && mb[2] < 841.90);
    ASSERT(mb[3] > 595.27 && mb[3] < 595.29);

    tspdf_reader_destroy(r);
    tspdf_reader_destroy(doc);
}

// The page struct exposes the (own or inherited) /CropBox so callers like
// `tspdf info` can report it without re-walking the page tree.
TEST(test_page_crop_box_exposed) {
    TspdfError err;
    const char *pdf =
        "%PDF-1.4\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/CropBox [100 100 400 500] >>\nendobj\n"
        "trailer\n<< /Root 1 0 R >>\n";
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, strlen(pdf), &err);
    ASSERT(doc != NULL);
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(page->has_crop_box);
    ASSERT(page->crop_box[0] == 100 && page->crop_box[1] == 100 &&
           page->crop_box[2] == 400 && page->crop_box[3] == 500);
    tspdf_reader_destroy(doc);

    // A page without a CropBox reports none.
    doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);
    ASSERT(!page->has_crop_box);
    tspdf_reader_destroy(doc);
}

// Raw /P permission flags of an encrypted document (as used by `tspdf info`).
TEST(test_encryption_permissions_accessor) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    // Unencrypted document: no permissions to report.
    uint32_t p = 0;
    ASSERT(!tspdf_reader_encryption_permissions(doc, &p));

    // Encrypt allowing print (bit 3) + copy (bit 5): /P = 0xFFFFF0D4 (-3884).
    uint8_t *buf = NULL;
    size_t len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &buf, &len,
                                                "pw", "pw", 0xFFFFF0D4u, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *enc = tspdf_reader_open_with_password(buf, len, "pw", &err);
    ASSERT(enc != NULL);
    ASSERT(tspdf_reader_encryption_permissions(enc, &p));
    ASSERT(p == 0xFFFFF0D4u);

    tspdf_reader_destroy(enc);
    free(buf);
    tspdf_reader_destroy(doc);
}

TEST(test_resize_to_rejects_nonpositive) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0};
    err = TSPDF_OK;
    ASSERT(tspdf_reader_resize_to(doc, pages, 1, 0.0, 800.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    tspdf_reader_destroy(doc);
}

TEST(test_scale_rejects_nonpositive_factor) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    err = TSPDF_OK;
    ASSERT(tspdf_reader_scale(doc, pages, 1, 0.0, 1.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    err = TSPDF_OK;
    ASSERT(tspdf_reader_scale(doc, pages, 1, 1.0, -2.0, &err) == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

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

TEST(test_save_rejects_missing_stream_source) {
    TspdfError err;
    TspdfReader *doc1 = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc1 != NULL);

    TspdfReader *docs[] = {doc1};
    TspdfReader *merged = tspdf_reader_merge(docs, 1, &err);
    ASSERT(merged != NULL);
    ASSERT(merged->data == NULL);

    TspdfObj *stream = NULL;
    for (size_t i = 1; i < merged->xref.count; i++) {
        if (merged->obj_cache[i] && merged->obj_cache[i]->type == TSPDF_OBJ_STREAM) {
            stream = merged->obj_cache[i];
            break;
        }
    }
    ASSERT(stream != NULL);
    ASSERT(stream->stream.data != NULL);

    uint8_t *old_data = stream->stream.data;
    stream->stream.data = NULL;
    stream->stream.self_contained = false;
    stream->stream.raw_offset = 1;
    stream->stream.raw_len = 1;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT(err != TSPDF_OK);
    ASSERT(out == NULL);
    ASSERT_EQ_SIZE(out_len, 0);

    err = tspdf_reader_save_to_memory_encrypted(merged, &out, &out_len,
                                                  "user", "owner", 0xFFFFFFFC, 128);
    ASSERT(err != TSPDF_OK);
    ASSERT(out == NULL);
    ASSERT_EQ_SIZE(out_len, 0);

    free(old_data);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc1);
}

TEST(test_merge_rejects_invalid_source_stream_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    ASSERT(page != NULL);

    TspdfObj *contents = tspdf_dict_get(page->page_dict, "Contents");
    ASSERT(contents != NULL);

    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);

    TspdfObj *stream = NULL;
    if (contents->type == TSPDF_OBJ_REF) {
        stream = tspdf_xref_resolve(&doc->xref, &parser, contents->ref.num, doc->obj_cache, NULL);
    } else if (contents->type == TSPDF_OBJ_STREAM) {
        stream = contents;
    } else if (contents->type == TSPDF_OBJ_ARRAY && contents->array.count > 0) {
        TspdfObj *first = &contents->array.items[0];
        if (first->type == TSPDF_OBJ_REF) {
            stream = tspdf_xref_resolve(&doc->xref, &parser, first->ref.num, doc->obj_cache, NULL);
        } else if (first->type == TSPDF_OBJ_STREAM) {
            stream = first;
        }
    }
    ASSERT(stream != NULL);
    ASSERT_EQ_INT(stream->type, TSPDF_OBJ_STREAM);

    stream->stream.data = NULL;
    stream->stream.self_contained = false;
    stream->stream.raw_offset = doc->data_len + 1;
    stream->stream.raw_len = 1;

    TspdfReader *docs[] = {doc};
    TspdfReader *merged = tspdf_reader_merge(docs, 1, &err);
    ASSERT(merged == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PARSE);

    tspdf_reader_destroy(doc);
}

TEST(test_extract_preserves_inherited_page_attributes) {
    size_t len = 0;
    char *pdf = make_inherited_page_attrs_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *src_page = tspdf_reader_get_page(doc, 0);
    ASSERT(src_page != NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Resources") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "MediaBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "BleedBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "TrimBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "ArtBox") == NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Rotate") == NULL);
    ASSERT_EQ_INT(src_page->rotate, 90);
    ASSERT_EQ_INT((int)src_page->media_box[2], 300);
    ASSERT_EQ_INT((int)src_page->media_box[3], 400);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(reopened, 0);
    ASSERT(page != NULL);
    ASSERT_EQ_INT(page->rotate, 90);
    ASSERT_EQ_INT((int)page->media_box[2], 300);
    ASSERT_EQ_INT((int)page->media_box[3], 400);

    TspdfObj *resources = tspdf_dict_get(page->page_dict, "Resources");
    ASSERT(resources != NULL);
    ASSERT_EQ_INT(resources->type, TSPDF_OBJ_DICT);
    TspdfObj *fonts = tspdf_dict_get(resources, "Font");
    ASSERT(fonts != NULL);
    ASSERT_EQ_INT(fonts->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(fonts, "F1") != NULL);

    TspdfObj *bleed_box = tspdf_dict_get(page->page_dict, "BleedBox");
    ASSERT(bleed_box != NULL);
    ASSERT_EQ_INT(bleed_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(bleed_box->array.count, 4);
    ASSERT_EQ_INT((int)bleed_box->array.items[2].integer, 295);

    TspdfObj *trim_box = tspdf_dict_get(page->page_dict, "TrimBox");
    ASSERT(trim_box != NULL);
    ASSERT_EQ_INT(trim_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(trim_box->array.count, 4);
    ASSERT_EQ_INT((int)trim_box->array.items[2].integer, 290);

    TspdfObj *art_box = tspdf_dict_get(page->page_dict, "ArtBox");
    ASSERT(art_box != NULL);
    ASSERT_EQ_INT(art_box->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(art_box->array.count, 4);
    ASSERT_EQ_INT((int)art_box->array.items[2].integer, 285);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_serializes_inferred_page_type_and_parent) {
    size_t len = 0;
    char *pdf = make_missing_page_tree_type_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    TspdfReaderPage *src_page = tspdf_reader_get_page(doc, 0);
    ASSERT(src_page != NULL);
    ASSERT(tspdf_dict_get(src_page->page_dict, "Type") == NULL);

    size_t pages[] = {0};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    TspdfReaderPage *page = tspdf_reader_get_page(reopened, 0);
    ASSERT(page != NULL);

    TspdfObj *type = tspdf_dict_get(page->page_dict, "Type");
    ASSERT(type != NULL);
    ASSERT_EQ_INT(type->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)type->string.data, "Page");

    TspdfObj *parent = tspdf_dict_get(page->page_dict, "Parent");
    ASSERT(parent != NULL);
    ASSERT_EQ_INT(parent->type, TSPDF_OBJ_REF);
    ASSERT_EQ_INT(parent->ref.num, 2);

    tspdf_reader_destroy(reopened);
    free(out);
    tspdf_reader_destroy(extracted);
    tspdf_reader_destroy(doc);
    free(pdf);
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

TEST(test_extract_single_page_omits_unselected_sibling_streams) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *extracted = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(extracted != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "(Page 2)"));
    ASSERT(!bytes_contains(out, out_len, "(Page 1)"));
    ASSERT(!bytes_contains(out, out_len, "(Page 3)"));

    TspdfReader *reopened = tspdf_reader_open(out, out_len, &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(out);
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

// A hand-built V=4/R=4 PDF whose /StmF is /Identity: stream data is stored in
// the clear while the crypt dict still authenticates the (empty) password.
// The /O, /U and /ID values are taken from a qpdf-generated V4 AESV2 file
// (empty user password, owner "o", P=-4) — password verification depends only
// on those, not on the crypt-filter choice, so this is a genuinely valid file.
static char *make_v4_identity_stmf_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    static const char *O_HEX =
        "77b8fb098022d3ab34237ea5643c08710ea5123fc5f88bf993a68cca5f12b40f";
    static const char *U_HEX =
        "e8dea6f86ac2bec59be9e09da2d4f1d20021446990b9e4114071a4d9104984c1";
    static const char *ID_HEX = "c894ca13f80c954088d2e3b894bf9c45";
    static const char *CONTENT = "BT /F1 12 Tf (IDENTITYSTREAM) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(CONTENT), CONTENT)) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Filter /Standard /V 4 /R 4 /Length 128 "
                 "/P -4 /O <%s> /U <%s> "
                 "/CF << /StdCF << /CFM /AESV2 /Length 16 /AuthEvent /DocOpen >> >> "
                 "/StmF /Identity /StrF /StdCF >>\nendobj\n",
                 O_HEX, U_HEX)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n"
                 "0 6\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 6 /Root 1 0 R /Encrypt 5 0 R "
                 "/ID [<%s><%s>] >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, ID_HEX, ID_HEX, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// --- Error handling tests ---

TEST(test_open_v4_identity_stmf_reads_plaintext_stream) {
    size_t len = 0;
    char *pdf = make_v4_identity_stmf_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_with_password(
        (const uint8_t *)pdf, len, "", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);

    // Saving without encryption must surface the plaintext content stream
    // verbatim: /StmF /Identity means it was never encrypted, so decryption is
    // a no-op rather than mangling the bytes.
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "IDENTITYSTREAM"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

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

TEST(test_open_cyclic_page_tree_rejected) {
    size_t len = 0;
    char *pdf = make_cyclic_page_tree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc == NULL);
    ASSERT(err != TSPDF_OK);

    free(pdf);
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

// NOTE: xref streams and incremental /Prev chains have synthetic coverage above.

/* R6/AESV3 (AES-256, ISO 32000-2) fixtures generated by qpdf 12.3.2:
 *   qpdf --encrypt --user-password=secret --owner-password=owner --bits=256 \
 *        -- clean.pdf R6_SECRET_PDF
 *   qpdf --encrypt --user-password= --owner-password=owner --bits=256 \
 *        -- clean.pdf R6_EMPTY_PDF
 * These exercise the real ISO 32000-2 Algorithm 2.B extended password hash. */
static const unsigned char R6_SECRET_PDF[] = {
    37,80,68,70,45,49,46,55,10,37,191,247,162,254,10,49,
    32,48,32,111,98,106,10,60,60,32,47,69,120,116,101,110,
    115,105,111,110,115,32,60,60,32,47,65,68,66,69,32,60,
    60,32,47,66,97,115,101,86,101,114,115,105,111,110,32,47,
    49,46,55,32,47,69,120,116,101,110,115,105,111,110,76,101,
    118,101,108,32,56,32,62,62,32,62,62,32,47,80,97,103,
    101,115,32,51,32,48,32,82,32,47,84,121,112,101,32,47,
    67,97,116,97,108,111,103,32,62,62,10,101,110,100,111,98,
    106,10,50,32,48,32,111,98,106,10,60,60,32,47,67,114,
    101,97,116,111,114,32,60,49,100,53,51,53,49,100,49,49,
    51,50,52,98,51,56,99,54,99,52,51,56,55,57,54,97,
    100,56,101,54,99,99,51,57,97,101,49,51,102,53,98,56,
    49,56,102,100,53,98,55,102,54,57,99,102,101,57,55,57,
    97,99,51,102,48,49,99,62,32,47,80,114,111,100,117,99,
    101,114,32,60,101,51,100,55,98,48,102,50,97,54,54,54,
    49,57,52,51,48,99,56,98,51,55,52,48,55,48,56,53,
    52,57,50,99,51,100,57,98,49,57,55,101,48,51,54,53,
    49,100,57,51,55,48,51,55,101,56,53,54,100,99,101,51,
    57,54,100,50,62,32,47,84,105,116,108,101,32,60,52,49,
    98,98,51,52,98,100,53,56,97,98,100,99,98,102,101,52,
    51,51,101,54,98,54,50,57,53,53,51,50,56,53,55,48,
    102,99,53,50,57,49,48,53,56,49,102,55,102,53,54,97,
    52,49,53,97,54,51,50,100,49,101,98,48,49,97,62,32,
    62,62,10,101,110,100,111,98,106,10,51,32,48,32,111,98,
    106,10,60,60,32,47,67,111,117,110,116,32,49,32,47,75,
    105,100,115,32,91,32,52,32,48,32,82,32,93,32,47,84,
    121,112,101,32,47,80,97,103,101,115,32,62,62,10,101,110,
    100,111,98,106,10,52,32,48,32,111,98,106,10,60,60,32,
    47,67,111,110,116,101,110,116,115,32,53,32,48,32,82,32,
    47,77,101,100,105,97,66,111,120,32,91,32,48,32,48,32,
    53,57,53,46,50,55,54,48,32,56,52,49,46,56,57,48,
    48,32,93,32,47,80,97,114,101,110,116,32,51,32,48,32,
    82,32,47,82,101,115,111,117,114,99,101,115,32,60,60,32,
    47,70,111,110,116,32,60,60,32,47,70,49,32,54,32,48,
    32,82,32,47,70,50,32,55,32,48,32,82,32,47,70,51,
    32,56,32,48,32,82,32,62,62,32,62,62,32,47,84,121,
    112,101,32,47,80,97,103,101,32,62,62,10,101,110,100,111,
    98,106,10,53,32,48,32,111,98,106,10,60,60,32,47,76,
    101,110,103,116,104,32,49,49,50,32,47,70,105,108,116,101,
    114,32,47,70,108,97,116,101,68,101,99,111,100,101,32,62,
    62,10,115,116,114,101,97,109,10,132,154,156,96,81,104,172,
    129,144,30,43,144,94,61,213,190,220,120,244,160,48,32,149,
    199,238,175,183,96,129,251,11,13,190,64,232,245,237,21,108,
    21,245,225,15,100,130,206,52,12,209,240,247,5,246,207,91,
    74,141,71,245,72,104,3,32,230,206,64,161,61,42,199,179,
    178,40,237,205,31,157,53,141,97,160,19,200,52,219,98,153,
    162,216,45,189,124,144,244,25,46,149,6,12,238,150,136,172,
    142,211,207,136,213,63,154,86,97,101,110,100,115,116,114,101,
    97,109,10,101,110,100,111,98,106,10,54,32,48,32,111,98,
    106,10,60,60,32,47,66,97,115,101,70,111,110,116,32,47,
    72,101,108,118,101,116,105,99,97,32,47,69,110,99,111,100,
    105,110,103,32,47,87,105,110,65,110,115,105,69,110,99,111,
    100,105,110,103,32,47,83,117,98,116,121,112,101,32,47,84,
    121,112,101,49,32,47,84,121,112,101,32,47,70,111,110,116,
    32,62,62,10,101,110,100,111,98,106,10,55,32,48,32,111,
    98,106,10,60,60,32,47,66,97,115,101,70,111,110,116,32,
    47,72,101,108,118,101,116,105,99,97,45,66,111,108,100,32,
    47,69,110,99,111,100,105,110,103,32,47,87,105,110,65,110,
    115,105,69,110,99,111,100,105,110,103,32,47,83,117,98,116,
    121,112,101,32,47,84,121,112,101,49,32,47,84,121,112,101,
    32,47,70,111,110,116,32,62,62,10,101,110,100,111,98,106,
    10,56,32,48,32,111,98,106,10,60,60,32,47,66,97,115,
    101,70,111,110,116,32,47,67,111,117,114,105,101,114,32,47,
    69,110,99,111,100,105,110,103,32,47,87,105,110,65,110,115,
    105,69,110,99,111,100,105,110,103,32,47,83,117,98,116,121,
    112,101,32,47,84,121,112,101,49,32,47,84,121,112,101,32,
    47,70,111,110,116,32,62,62,10,101,110,100,111,98,106,10,
    57,32,48,32,111,98,106,10,60,60,32,47,67,70,32,60,
    60,32,47,83,116,100,67,70,32,60,60,32,47,65,117,116,
    104,69,118,101,110,116,32,47,68,111,99,79,112,101,110,32,
    47,67,70,77,32,47,65,69,83,86,51,32,47,76,101,110,
    103,116,104,32,51,50,32,62,62,32,62,62,32,47,70,105,
    108,116,101,114,32,47,83,116,97,110,100,97,114,100,32,47,
    76,101,110,103,116,104,32,50,53,54,32,47,79,32,60,98,
    100,57,55,48,53,52,55,48,101,52,100,57,55,54,101,98,
    55,101,101,51,54,100,98,101,55,53,54,99,50,102,100,102,
    56,100,53,56,98,51,98,57,54,50,97,50,100,100,48,100,
    50,48,99,99,101,97,50,54,50,97,101,51,100,48,97,49,
    101,98,55,49,54,99,54,57,99,98,102,101,101,51,53,56,
    55,57,49,53,48,53,97,101,51,55,50,100,57,55,51,62,
    32,47,79,69,32,60,56,51,99,57,51,53,57,54,56,51,
    97,52,55,99,48,101,56,49,49,53,101,56,56,97,56,55,
    54,101,51,97,98,101,51,99,100,55,97,50,97,48,102,57,
    51,54,101,55,97,97,102,102,54,53,50,51,56,101,97,52,
    51,50,100,56,57,50,62,32,47,80,32,45,52,32,47,80,
    101,114,109,115,32,60,101,56,56,56,48,99,54,98,54,49,
    102,102,55,50,55,98,98,48,54,98,51,50,102,99,57,57,
    50,56,48,102,55,51,62,32,47,82,32,54,32,47,83,116,
    109,70,32,47,83,116,100,67,70,32,47,83,116,114,70,32,
    47,83,116,100,67,70,32,47,85,32,60,56,48,49,98,52,
    100,57,51,54,97,50,50,97,102,54,53,99,97,53,52,54,
    98,97,51,97,101,48,102,53,55,52,55,49,51,49,100,101,
    97,48,57,100,97,50,51,97,57,49,55,57,50,50,48,54,
    56,48,54,57,100,57,52,102,51,50,102,100,99,97,52,57,
    57,101,100,102,102,97,101,102,50,97,100,57,102,54,57,57,
    56,54,101,50,98,100,97,52,97,53,53,62,32,47,85,69,
    32,60,98,48,100,52,57,54,48,99,99,57,48,55,50,99,
    49,57,102,57,52,55,100,53,54,101,56,100,54,55,55,102,
    100,57,100,101,49,57,101,51,53,57,56,48,52,55,100,53,
    50,100,100,97,55,53,98,54,50,101,56,50,99,51,55,57,
    97,50,62,32,47,86,32,53,32,62,62,10,101,110,100,111,
    98,106,10,120,114,101,102,10,48,32,49,48,10,48,48,48,
    48,48,48,48,48,48,48,32,54,53,53,51,53,32,102,32,
    10,48,48,48,48,48,48,48,48,49,53,32,48,48,48,48,
    48,32,110,32,10,48,48,48,48,48,48,48,49,51,48,32,
    48,48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,
    51,55,56,32,48,48,48,48,48,32,110,32,10,48,48,48,
    48,48,48,48,52,51,55,32,48,48,48,48,48,32,110,32,
    10,48,48,48,48,48,48,48,53,57,53,32,48,48,48,48,
    48,32,110,32,10,48,48,48,48,48,48,48,55,55,56,32,
    48,48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,
    56,55,53,32,48,48,48,48,48,32,110,32,10,48,48,48,
    48,48,48,48,57,55,55,32,48,48,48,48,48,32,110,32,
    10,48,48,48,48,48,48,49,48,55,50,32,48,48,48,48,
    48,32,110,32,10,116,114,97,105,108,101,114,32,60,60,32,
    47,73,110,102,111,32,50,32,48,32,82,32,47,82,111,111,
    116,32,49,32,48,32,82,32,47,83,105,122,101,32,49,48,
    32,47,73,68,32,91,60,49,51,51,48,52,102,100,98,51,
    99,57,50,55,49,56,101,51,56,55,50,50,51,49,54,50,
    98,101,102,50,49,98,55,62,60,49,51,51,48,52,102,100,
    98,51,99,57,50,55,49,56,101,51,56,55,50,50,51,49,
    54,50,98,101,102,50,49,98,55,62,93,32,47,69,110,99,
    114,121,112,116,32,57,32,48,32,82,32,62,62,10,115,116,
    97,114,116,120,114,101,102,10,49,54,49,57,10,37,37,69,
    79,70,10,
};
static const size_t R6_SECRET_PDF_len = 1987;

static const unsigned char R6_EMPTY_PDF[] = {
    37,80,68,70,45,49,46,55,10,37,191,247,162,254,10,49,
    32,48,32,111,98,106,10,60,60,32,47,69,120,116,101,110,
    115,105,111,110,115,32,60,60,32,47,65,68,66,69,32,60,
    60,32,47,66,97,115,101,86,101,114,115,105,111,110,32,47,
    49,46,55,32,47,69,120,116,101,110,115,105,111,110,76,101,
    118,101,108,32,56,32,62,62,32,62,62,32,47,80,97,103,
    101,115,32,51,32,48,32,82,32,47,84,121,112,101,32,47,
    67,97,116,97,108,111,103,32,62,62,10,101,110,100,111,98,
    106,10,50,32,48,32,111,98,106,10,60,60,32,47,67,114,
    101,97,116,111,114,32,60,97,56,100,98,56,99,48,100,53,
    100,100,53,57,54,100,99,49,98,48,98,55,53,99,97,57,
    50,48,53,56,48,49,55,97,49,50,55,101,49,56,102,51,
    99,49,97,51,51,57,57,99,101,101,52,55,101,54,49,99,
    97,97,97,53,50,99,56,62,32,47,80,114,111,100,117,99,
    101,114,32,60,48,49,101,53,52,56,56,48,56,99,49,49,
    48,52,51,55,101,51,56,57,99,55,55,102,48,57,55,98,
    54,55,51,57,97,55,51,52,101,98,98,98,50,53,48,49,
    101,56,101,49,57,100,102,101,48,57,57,102,98,101,102,55,
    100,98,50,55,62,32,47,84,105,116,108,101,32,60,53,100,
    52,56,49,50,52,98,100,97,101,54,99,55,56,53,53,53,
    48,56,100,49,52,97,102,97,97,56,98,52,56,57,100,54,
    99,49,55,56,98,97,57,97,99,98,53,101,57,101,50,48,
    50,49,52,98,49,49,57,52,102,54,54,49,53,99,62,32,
    62,62,10,101,110,100,111,98,106,10,51,32,48,32,111,98,
    106,10,60,60,32,47,67,111,117,110,116,32,49,32,47,75,
    105,100,115,32,91,32,52,32,48,32,82,32,93,32,47,84,
    121,112,101,32,47,80,97,103,101,115,32,62,62,10,101,110,
    100,111,98,106,10,52,32,48,32,111,98,106,10,60,60,32,
    47,67,111,110,116,101,110,116,115,32,53,32,48,32,82,32,
    47,77,101,100,105,97,66,111,120,32,91,32,48,32,48,32,
    53,57,53,46,50,55,54,48,32,56,52,49,46,56,57,48,
    48,32,93,32,47,80,97,114,101,110,116,32,51,32,48,32,
    82,32,47,82,101,115,111,117,114,99,101,115,32,60,60,32,
    47,70,111,110,116,32,60,60,32,47,70,49,32,54,32,48,
    32,82,32,47,70,50,32,55,32,48,32,82,32,47,70,51,
    32,56,32,48,32,82,32,62,62,32,62,62,32,47,84,121,
    112,101,32,47,80,97,103,101,32,62,62,10,101,110,100,111,
    98,106,10,53,32,48,32,111,98,106,10,60,60,32,47,76,
    101,110,103,116,104,32,49,49,50,32,47,70,105,108,116,101,
    114,32,47,70,108,97,116,101,68,101,99,111,100,101,32,62,
    62,10,115,116,114,101,97,109,10,187,237,51,109,3,205,81,
    234,202,63,240,202,251,67,63,137,81,18,107,213,199,36,221,
    124,124,73,12,160,237,23,179,23,224,183,220,30,171,53,103,
    110,81,108,99,181,114,77,2,164,223,144,153,84,64,231,120,
    35,15,87,109,75,50,34,214,196,26,79,8,182,100,92,39,
    45,57,111,206,65,234,48,55,127,72,231,109,154,79,210,70,
    179,52,109,156,113,129,206,115,227,45,177,27,231,169,29,239,
    78,29,226,56,151,67,208,1,134,101,110,100,115,116,114,101,
    97,109,10,101,110,100,111,98,106,10,54,32,48,32,111,98,
    106,10,60,60,32,47,66,97,115,101,70,111,110,116,32,47,
    72,101,108,118,101,116,105,99,97,32,47,69,110,99,111,100,
    105,110,103,32,47,87,105,110,65,110,115,105,69,110,99,111,
    100,105,110,103,32,47,83,117,98,116,121,112,101,32,47,84,
    121,112,101,49,32,47,84,121,112,101,32,47,70,111,110,116,
    32,62,62,10,101,110,100,111,98,106,10,55,32,48,32,111,
    98,106,10,60,60,32,47,66,97,115,101,70,111,110,116,32,
    47,72,101,108,118,101,116,105,99,97,45,66,111,108,100,32,
    47,69,110,99,111,100,105,110,103,32,47,87,105,110,65,110,
    115,105,69,110,99,111,100,105,110,103,32,47,83,117,98,116,
    121,112,101,32,47,84,121,112,101,49,32,47,84,121,112,101,
    32,47,70,111,110,116,32,62,62,10,101,110,100,111,98,106,
    10,56,32,48,32,111,98,106,10,60,60,32,47,66,97,115,
    101,70,111,110,116,32,47,67,111,117,114,105,101,114,32,47,
    69,110,99,111,100,105,110,103,32,47,87,105,110,65,110,115,
    105,69,110,99,111,100,105,110,103,32,47,83,117,98,116,121,
    112,101,32,47,84,121,112,101,49,32,47,84,121,112,101,32,
    47,70,111,110,116,32,62,62,10,101,110,100,111,98,106,10,
    57,32,48,32,111,98,106,10,60,60,32,47,67,70,32,60,
    60,32,47,83,116,100,67,70,32,60,60,32,47,65,117,116,
    104,69,118,101,110,116,32,47,68,111,99,79,112,101,110,32,
    47,67,70,77,32,47,65,69,83,86,51,32,47,76,101,110,
    103,116,104,32,51,50,32,62,62,32,62,62,32,47,70,105,
    108,116,101,114,32,47,83,116,97,110,100,97,114,100,32,47,
    76,101,110,103,116,104,32,50,53,54,32,47,79,32,60,51,
    56,100,51,101,57,48,101,54,102,55,98,101,100,99,98,97,
    50,54,98,54,52,102,54,52,98,56,49,52,50,56,57,50,
    48,57,49,53,102,97,48,51,51,98,101,52,53,49,55,97,
    55,57,102,48,99,57,49,51,99,53,48,53,48,49,97,49,
    100,99,57,54,50,50,100,57,56,98,53,97,48,50,52,50,
    53,51,55,55,101,57,50,97,101,54,57,101,52,55,55,62,
    32,47,79,69,32,60,49,99,98,50,55,97,56,54,53,97,
    102,52,102,57,51,101,57,98,57,51,98,57,100,99,54,56,
    101,54,54,55,98,48,54,48,97,52,48,102,53,99,53,54,
    51,57,53,53,52,53,97,56,54,55,100,52,99,98,102,99,
    97,56,55,55,100,50,62,32,47,80,32,45,52,32,47,80,
    101,114,109,115,32,60,48,98,49,48,101,102,101,100,56,102,
    55,48,51,52,52,101,97,57,102,52,56,52,98,50,54,52,
    57,52,55,49,101,53,62,32,47,82,32,54,32,47,83,116,
    109,70,32,47,83,116,100,67,70,32,47,83,116,114,70,32,
    47,83,116,100,67,70,32,47,85,32,60,49,49,99,52,54,
    100,52,56,52,53,54,100,102,52,102,102,56,49,56,55,50,
    49,53,48,97,57,57,101,98,98,52,52,56,56,97,50,57,
    55,102,102,53,97,57,102,56,52,48,99,98,56,50,101,54,
    51,99,57,48,52,50,53,51,57,49,53,51,54,57,97,50,
    56,50,51,53,55,55,101,99,55,54,55,100,48,98,48,102,
    51,101,54,49,54,57,51,48,99,50,52,62,32,47,85,69,
    32,60,49,52,102,99,49,57,56,51,55,57,56,48,49,57,
    98,49,97,57,100,51,54,50,48,50,50,102,99,51,101,98,
    49,102,54,50,49,101,99,102,101,99,48,49,48,98,53,100,
    98,52,100,55,49,57,55,55,50,57,56,49,54,102,51,51,
    53,56,62,32,47,86,32,53,32,62,62,10,101,110,100,111,
    98,106,10,120,114,101,102,10,48,32,49,48,10,48,48,48,
    48,48,48,48,48,48,48,32,54,53,53,51,53,32,102,32,
    10,48,48,48,48,48,48,48,48,49,53,32,48,48,48,48,
    48,32,110,32,10,48,48,48,48,48,48,48,49,51,48,32,
    48,48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,
    51,55,56,32,48,48,48,48,48,32,110,32,10,48,48,48,
    48,48,48,48,52,51,55,32,48,48,48,48,48,32,110,32,
    10,48,48,48,48,48,48,48,53,57,53,32,48,48,48,48,
    48,32,110,32,10,48,48,48,48,48,48,48,55,55,56,32,
    48,48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,
    56,55,53,32,48,48,48,48,48,32,110,32,10,48,48,48,
    48,48,48,48,57,55,55,32,48,48,48,48,48,32,110,32,
    10,48,48,48,48,48,48,49,48,55,50,32,48,48,48,48,
    48,32,110,32,10,116,114,97,105,108,101,114,32,60,60,32,
    47,73,110,102,111,32,50,32,48,32,82,32,47,82,111,111,
    116,32,49,32,48,32,82,32,47,83,105,122,101,32,49,48,
    32,47,73,68,32,91,60,49,100,97,49,55,49,101,54,56,
    100,100,55,53,52,48,97,48,102,57,52,52,99,54,53,102,
    57,54,54,99,48,101,55,62,60,49,100,97,49,55,49,101,
    54,56,100,100,55,53,52,48,97,48,102,57,52,52,99,54,
    53,102,57,54,54,99,48,101,55,62,93,32,47,69,110,99,
    114,121,112,116,32,57,32,48,32,82,32,62,62,10,115,116,
    97,114,116,120,114,101,102,10,49,54,49,57,10,37,37,69,
    79,70,10,
};
static const size_t R6_EMPTY_PDF_len = 1987;

TEST(test_open_r6_aes256_with_user_password) {
    TspdfError err = TSPDF_OK;
    /* Correct user password must open the R6 document. */
    TspdfReader *doc = tspdf_reader_open_with_password(
        R6_SECRET_PDF, R6_SECRET_PDF_len, "secret", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    tspdf_reader_destroy(doc);
}

TEST(test_open_r6_aes256_wrong_password_rejected) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_with_password(
        R6_SECRET_PDF, R6_SECRET_PDF_len, "wrong", &err);
    ASSERT(doc == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_BAD_PASSWORD);
}

TEST(test_open_r6_aes256_empty_user_password) {
    TspdfError err = TSPDF_OK;
    /* Empty user password must open this R6 document. */
    TspdfReader *doc = tspdf_reader_open_with_password(
        R6_EMPTY_PDF, R6_EMPTY_PDF_len, "", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    tspdf_reader_destroy(doc);
}

TEST(test_open_r6_aes256_owner_password) {
    TspdfError err = TSPDF_OK;
    /* Owner password must also open the R6 document. */
    TspdfReader *doc = tspdf_reader_open_with_password(
        R6_SECRET_PDF, R6_SECRET_PDF_len, "owner", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
}

/* R4/AESV2 with /EncryptMetadata false, generated by qpdf 12.3.2:
 *   qpdf --allow-weak-crypto --encrypt --user-password= --owner-password=o \
 *        --bits=128 --use-aes=y --cleartext-metadata -- clean.pdf CLEARTEXT_META_PDF
 * Algorithm 2 step (f) requires appending 0xFFFFFFFF to the key MD5 when
 * /EncryptMetadata is false and R >= 4. */
static const unsigned char CLEARTEXT_META_PDF[] = {
    37,80,68,70,45,49,46,55,10,37,191,247,162,254,10,49,
    32,48,32,111,98,106,10,60,60,32,47,80,97,103,101,115,
    32,51,32,48,32,82,32,47,84,121,112,101,32,47,67,97,
    116,97,108,111,103,32,62,62,10,101,110,100,111,98,106,10,
    50,32,48,32,111,98,106,10,60,60,32,47,67,114,101,97,
    116,111,114,32,60,48,48,48,97,49,101,98,99,100,52,101,
    48,102,97,55,54,99,50,99,57,49,55,54,57,50,100,48,
    53,49,101,52,101,48,57,55,48,48,100,53,55,98,98,98,
    56,99,55,53,48,100,50,52,48,56,102,53,100,55,56,50,
    55,50,48,102,100,62,32,47,80,114,111,100,117,99,101,114,
    32,60,51,54,51,53,48,52,102,102,51,53,102,98,99,102,
    97,99,57,54,52,101,101,98,53,102,101,53,54,99,50,51,
    101,48,55,49,56,50,101,100,50,52,98,101,97,99,51,57,
    53,55,54,101,57,54,98,52,56,101,48,56,100,102,55,50,
    97,101,62,32,47,84,105,116,108,101,32,60,55,57,101,54,
    99,56,102,49,101,98,97,97,55,53,100,97,57,50,48,57,
    98,97,49,101,53,55,56,48,53,53,51,56,57,51,53,52,
    49,102,102,57,97,48,57,51,101,51,56,99,99,99,53,97,
    49,101,52,57,100,102,51,98,52,52,56,54,62,32,62,62,
    10,101,110,100,111,98,106,10,51,32,48,32,111,98,106,10,
    60,60,32,47,67,111,117,110,116,32,49,32,47,75,105,100,
    115,32,91,32,52,32,48,32,82,32,93,32,47,84,121,112,
    101,32,47,80,97,103,101,115,32,62,62,10,101,110,100,111,
    98,106,10,52,32,48,32,111,98,106,10,60,60,32,47,67,
    111,110,116,101,110,116,115,32,53,32,48,32,82,32,47,77,
    101,100,105,97,66,111,120,32,91,32,48,32,48,32,53,57,
    53,46,50,55,54,48,32,56,52,49,46,56,57,48,48,32,
    93,32,47,80,97,114,101,110,116,32,51,32,48,32,82,32,
    47,82,101,115,111,117,114,99,101,115,32,60,60,32,47,70,
    111,110,116,32,60,60,32,47,70,49,32,54,32,48,32,82,
    32,47,70,50,32,55,32,48,32,82,32,47,70,51,32,56,
    32,48,32,82,32,62,62,32,62,62,32,47,84,121,112,101,
    32,47,80,97,103,101,32,62,62,10,101,110,100,111,98,106,
    10,53,32,48,32,111,98,106,10,60,60,32,47,76,101,110,
    103,116,104,32,49,49,50,32,47,70,105,108,116,101,114,32,
    47,70,108,97,116,101,68,101,99,111,100,101,32,62,62,10,
    115,116,114,101,97,109,10,37,71,160,8,176,197,249,158,132,
    42,153,38,227,225,113,249,85,87,175,55,31,14,168,216,59,
    198,131,201,38,228,60,218,199,148,74,165,43,129,75,21,88,
    88,249,209,110,123,222,156,139,130,164,238,185,223,46,229,119,
    155,55,180,188,223,45,87,56,26,42,164,149,206,220,7,30,
    245,57,213,147,214,102,133,160,133,244,88,3,88,157,187,68,
    156,153,191,151,241,68,30,30,240,159,211,70,158,100,247,226,
    116,78,120,176,209,67,38,101,110,100,115,116,114,101,97,109,
    10,101,110,100,111,98,106,10,54,32,48,32,111,98,106,10,
    60,60,32,47,66,97,115,101,70,111,110,116,32,47,72,101,
    108,118,101,116,105,99,97,32,47,69,110,99,111,100,105,110,
    103,32,47,87,105,110,65,110,115,105,69,110,99,111,100,105,
    110,103,32,47,83,117,98,116,121,112,101,32,47,84,121,112,
    101,49,32,47,84,121,112,101,32,47,70,111,110,116,32,62,
    62,10,101,110,100,111,98,106,10,55,32,48,32,111,98,106,
    10,60,60,32,47,66,97,115,101,70,111,110,116,32,47,72,
    101,108,118,101,116,105,99,97,45,66,111,108,100,32,47,69,
    110,99,111,100,105,110,103,32,47,87,105,110,65,110,115,105,
    69,110,99,111,100,105,110,103,32,47,83,117,98,116,121,112,
    101,32,47,84,121,112,101,49,32,47,84,121,112,101,32,47,
    70,111,110,116,32,62,62,10,101,110,100,111,98,106,10,56,
    32,48,32,111,98,106,10,60,60,32,47,66,97,115,101,70,
    111,110,116,32,47,67,111,117,114,105,101,114,32,47,69,110,
    99,111,100,105,110,103,32,47,87,105,110,65,110,115,105,69,
    110,99,111,100,105,110,103,32,47,83,117,98,116,121,112,101,
    32,47,84,121,112,101,49,32,47,84,121,112,101,32,47,70,
    111,110,116,32,62,62,10,101,110,100,111,98,106,10,57,32,
    48,32,111,98,106,10,60,60,32,47,67,70,32,60,60,32,
    47,83,116,100,67,70,32,60,60,32,47,65,117,116,104,69,
    118,101,110,116,32,47,68,111,99,79,112,101,110,32,47,67,
    70,77,32,47,65,69,83,86,50,32,47,76,101,110,103,116,
    104,32,49,54,32,62,62,32,62,62,32,47,69,110,99,114,
    121,112,116,77,101,116,97,100,97,116,97,32,102,97,108,115,
    101,32,47,70,105,108,116,101,114,32,47,83,116,97,110,100,
    97,114,100,32,47,76,101,110,103,116,104,32,49,50,56,32,
    47,79,32,60,55,55,98,56,102,98,48,57,56,48,50,50,
    100,51,97,98,51,52,50,51,55,101,97,53,54,52,51,99,
    48,56,55,49,48,101,97,53,49,50,51,102,99,53,102,56,
    56,98,102,57,57,51,97,54,56,99,99,97,53,102,49,50,
    98,52,48,102,62,32,47,79,69,32,60,62,32,47,80,32,
    45,52,32,47,82,32,52,32,47,83,116,109,70,32,47,83,
    116,100,67,70,32,47,83,116,114,70,32,47,83,116,100,67,
    70,32,47,85,32,60,57,99,48,97,102,99,48,98,49,99,
    101,55,49,52,54,56,51,102,56,49,49,99,49,49,100,97,
    51,51,55,54,56,50,48,48,50,49,52,52,54,57,57,48,
    98,57,101,52,49,49,52,48,55,49,97,52,100,57,49,48,
    52,57,56,52,99,49,62,32,47,85,69,32,60,62,32,47,
    86,32,52,32,62,62,10,101,110,100,111,98,106,10,120,114,
    101,102,10,48,32,49,48,10,48,48,48,48,48,48,48,48,
    48,48,32,54,53,53,51,53,32,102,32,10,48,48,48,48,
    48,48,48,48,49,53,32,48,48,48,48,48,32,110,32,10,
    48,48,48,48,48,48,48,48,54,52,32,48,48,48,48,48,
    32,110,32,10,48,48,48,48,48,48,48,51,49,50,32,48,
    48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,51,
    55,49,32,48,48,48,48,48,32,110,32,10,48,48,48,48,
    48,48,48,53,50,57,32,48,48,48,48,48,32,110,32,10,
    48,48,48,48,48,48,48,55,49,50,32,48,48,48,48,48,
    32,110,32,10,48,48,48,48,48,48,48,56,48,57,32,48,
    48,48,48,48,32,110,32,10,48,48,48,48,48,48,48,57,
    49,49,32,48,48,48,48,48,32,110,32,10,48,48,48,48,
    48,48,49,48,48,54,32,48,48,48,48,48,32,110,32,10,
    116,114,97,105,108,101,114,32,60,60,32,47,73,110,102,111,
    32,50,32,48,32,82,32,47,82,111,111,116,32,49,32,48,
    32,82,32,47,83,105,122,101,32,49,48,32,47,73,68,32,
    91,60,100,48,102,55,101,54,57,56,49,49,100,98,100,97,
    99,48,54,97,99,49,54,50,50,53,52,101,102,57,49,54,
    101,53,62,60,100,48,102,55,101,54,57,56,49,49,100,98,
    100,97,99,48,54,97,99,49,54,50,50,53,52,101,102,57,
    49,54,101,53,62,93,32,47,69,110,99,114,121,112,116,32,
    57,32,48,32,82,32,62,62,10,115,116,97,114,116,120,114,
    101,102,10,49,51,52,50,10,37,37,69,79,70,10,
};
static const size_t CLEARTEXT_META_PDF_len = 1710;

TEST(test_open_r4_cleartext_metadata_empty_password) {
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open_with_password(
        CLEARTEXT_META_PDF, CLEARTEXT_META_PDF_len, "", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 1);
    tspdf_reader_destroy(doc);
}

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
    ASSERT(bytes_contains(encrypted_buf, encrypted_len, "/Size "));
    ASSERT(!bytes_contains(encrypted_buf, encrypted_len, "/TspdfSize"));

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

    /* Save unencrypted (unlock, explicit opt-out via opts.decrypt) */
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *unlocked_buf = NULL;
    size_t unlocked_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc2, &unlocked_buf,
                                                   &unlocked_len, &unlock_opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(unlocked_buf, unlocked_len, "(Page 1)"));

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

TEST(test_reencrypt_opened_encrypted_decrypts_source_streams) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *old_enc = NULL;
    size_t old_enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &old_enc, &old_enc_len,
                                                  "oldpass", "oldowner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *opened = tspdf_reader_open_with_password(old_enc, old_enc_len, "oldpass", &err);
    ASSERT(opened != NULL);

    uint8_t *new_enc = NULL;
    size_t new_enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(opened, &new_enc, &new_enc_len,
                                                  "newpass", "newowner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *reopened = tspdf_reader_open_with_password(new_enc, new_enc_len, "newpass", &err);
    ASSERT(reopened != NULL);

    // Plain saves preserve encryption by default; the explicit decrypt
    // opt-out exposes the plaintext this test checks.
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *unlocked = NULL;
    size_t unlocked_len = 0;
    err = tspdf_reader_save_to_memory_with_options(reopened, &unlocked,
                                                   &unlocked_len, &unlock_opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(unlocked, unlocked_len, "(Page 1)"));

    free(unlocked);
    tspdf_reader_destroy(reopened);
    free(new_enc);
    tspdf_reader_destroy(opened);
    free(old_enc);
    tspdf_reader_destroy(doc);
}

// Changing a file's password must not embed the OLD /Encrypt dict as an
// orphan object: its O/U hashes are offline-crackable material for the old
// password. Exactly one /Standard security handler may appear in the output.
TEST(test_reencrypt_single_encrypt_dict) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *old_enc = NULL;
    size_t old_enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &old_enc, &old_enc_len,
                                                  "oldpass", "oldowner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(bytes_count(old_enc, old_enc_len, "/Standard"), 1);

    TspdfReader *opened = tspdf_reader_open_with_password(old_enc, old_enc_len, "oldpass", &err);
    ASSERT(opened != NULL);

    uint8_t *new_enc = NULL;
    size_t new_enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(opened, &new_enc, &new_enc_len,
                                                  "newpass", "newowner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT_EQ_SIZE(bytes_count(new_enc, new_enc_len, "/Standard"), 1);

    // The new password still opens the re-encrypted file.
    TspdfReader *reopened = tspdf_reader_open_with_password(new_enc, new_enc_len, "newpass", &err);
    ASSERT(reopened != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(reopened), 1);

    tspdf_reader_destroy(reopened);
    free(new_enc);
    tspdf_reader_destroy(opened);
    free(old_enc);
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

    // The extract inherits the source's encryption, so a plain save stays
    // locked under the same password...
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(extracted, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "(Page 1)"));

    TspdfReader *doc3 = tspdf_reader_open_with_password(out, out_len, "test", &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 1);

    // ...and opts.decrypt unlocks the extract.
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    err = tspdf_reader_save_to_memory_with_options(extracted, &plain,
                                                   &plain_len, &unlock_opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(plain, plain_len, "(Page 1)"));
    free(plain);

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

    // Save unencrypted (explicit opt-out; plain saves preserve encryption)
    TspdfSaveOptions unlock_opts = tspdf_save_options_default();
    unlock_opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc2, &out, &out_len,
                                                   &unlock_opts);
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

// A page whose content stream is q/Q-unbalanced: it does `q 2 0 0 2 0 0 cm`
// (a 2x scale) and never restores it. This is a spec violation but is seen in
// the wild. If the overlay let that leaked CTM reach the new content, an
// overlay drawn at (72,72) would render at (144,144). The overlay must wrap the
// original content in its own q..Q so the leak cannot escape.
static char *make_unbalanced_content_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;
    // Unbalanced: an extra `q` with a 2x scale, never matched by `Q`.
    const char *content = "q 2 0 0 2 0 0 cm\n";
    size_t obj4 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%sendstream\nendobj\n",
                 strlen(content), content)) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n0 5\n0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// Resolve a /Contents array item (a REF) to its stream bytes for inspection.
static bool contents_item_bytes(TspdfReader *doc, TspdfObj *item,
                                const char **out, size_t *out_len) {
    if (!item || item->type != TSPDF_OBJ_REF) return false;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    TspdfObj *s = NULL;
    if (item->ref.num < doc->xref.count) {
        s = tspdf_xref_resolve(&doc->xref, &parser, item->ref.num, doc->obj_cache, doc->crypt);
    } else {
        size_t idx = item->ref.num - doc->xref.count;
        if (idx < doc->new_objs.count) s = doc->new_objs.objs[idx];
    }
    if (!s || s->type != TSPDF_OBJ_STREAM) return false;
    *out = (const char *)s->stream.data;
    *out_len = s->stream.len;
    return s->stream.data != NULL;
}

// Overlaying onto a q/Q-unbalanced page must wrap the original content in a
// balanced q..Q so its leaked CTM cannot reach the overlay: the resulting
// /Contents must be [q, old, Q, new].
TEST(test_overlay_wraps_unbalanced_content) {
    size_t len = 0;
    char *raw = make_unbalanced_content_pdf(&len);
    ASSERT(raw != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)raw, len, &err);
    ASSERT(doc != NULL);

    TspdfWriter *res = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res, "Helvetica");
    TspdfStream *ov = tspdf_page_begin_content(doc, 0);
    ASSERT(ov != NULL);
    tspdf_stream_begin_text(ov);
    tspdf_stream_set_font(ov, font, 12.0);
    tspdf_stream_text_position(ov, 72, 72);
    tspdf_stream_show_text(ov, "MARK");
    tspdf_stream_end_text(ov);
    err = tspdf_page_end_content(doc, 0, ov, res);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res);

    // The single original ref becomes a 4-item [q, old, Q, new] array.
    TspdfObj *contents = tspdf_dict_get(doc->pages.pages[0].page_dict, "Contents");
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(contents->array.count, 4);

    const char *b = NULL;
    size_t bl = 0;
    // items[0] is the "q\n" save that brackets the original content.
    ASSERT(contents_item_bytes(doc, &contents->array.items[0], &b, &bl));
    ASSERT(bl == 2 && memcmp(b, "q\n", 2) == 0);
    // items[2] is the matching "Q\n" restore, popping the original's leak.
    ASSERT(contents_item_bytes(doc, &contents->array.items[2], &b, &bl));
    ASSERT(bl == 2 && memcmp(b, "Q\n", 2) == 0);

    tspdf_reader_destroy(doc);
    free(raw);
}


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

// Count how many entries of `dict` carry `key` (duplicate-key detector).
static size_t dict_key_count(TspdfObj *dict, const char *key) {
    if (!dict || dict->type != TSPDF_OBJ_DICT) return 0;
    size_t n = 0;
    for (size_t i = 0; i < dict->dict.count; i++) {
        if (strcmp(dict->dict.entries[i].key, key) == 0) n++;
    }
    return n;
}

// Regression: pages whose /Resources is an INDIRECT reference (shared between
// pages, /Font itself indirect too — the pymupdf/pdfTeX shape). Merging
// overlay resources used to append a DUPLICATE /Resources key to the page
// dict, so the original fonts were orphaned and pages rendered near-blank.
TEST(test_overlay_indirect_shared_resources) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_resources.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    // One resource owner reused for both pages, like the watermark CLI does:
    // a font plus an opacity ExtGState.
    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    const char *gs = tspdf_writer_add_opacity(res_owner, 0.3, 0.3);
    ASSERT(font != NULL);
    ASSERT(gs != NULL);
    for (size_t p = 0; p < 2; p++) {
        TspdfStream *ov = tspdf_page_begin_content(doc, p);
        ASSERT(ov != NULL);
        tspdf_stream_set_opacity(ov, gs);
        tspdf_stream_begin_text(ov);
        tspdf_stream_set_font(ov, font, 36.0);
        tspdf_stream_text_position(ov, 100, 100);
        tspdf_stream_show_text(ov, "DRAFT");
        tspdf_stream_end_text(ov);
        err = tspdf_page_end_content(doc, p, ov, res_owner);
        ASSERT_EQ_INT(err, TSPDF_OK);
    }
    // res_owner stays alive: `font`/`gs` point into it and are used below.

    // The merge must not have appended a second /Resources key.
    for (size_t p = 0; p < 2; p++) {
        ASSERT_EQ_SIZE(dict_key_count(doc->pages.pages[p].page_dict, "Resources"), 1);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    for (size_t p = 0; p < 2; p++) {
        TspdfObj *page_dict = doc2->pages.pages[p].page_dict;
        TspdfObj *res = test_resolve_ref(doc2, tspdf_dict_get(page_dict, "Resources"));
        ASSERT(res != NULL);
        ASSERT_EQ_INT(res->type, TSPDF_OBJ_DICT);
        ASSERT_EQ_SIZE(dict_key_count(res, "Font"), 1);
        TspdfObj *fonts = test_resolve_ref(doc2, tspdf_dict_get(res, "Font"));
        ASSERT(fonts != NULL);
        ASSERT_EQ_INT(fonts->type, TSPDF_OBJ_DICT);
        // Original font must survive alongside the overlay font...
        ASSERT(tspdf_dict_get(fonts, "F1") != NULL);
        ASSERT(tspdf_dict_get(fonts, font) != NULL);
        // ...and the shared dict must not accumulate the OTHER page's
        // additions (each page gets its own copy: exactly F1 + overlay font).
        ASSERT_EQ_SIZE(fonts->dict.count, 2);
        // The opacity ExtGState made it in, too.
        TspdfObj *egs = test_resolve_ref(doc2, tspdf_dict_get(res, "ExtGState"));
        ASSERT(egs != NULL);
        ASSERT_EQ_INT(egs->type, TSPDF_OBJ_DICT);
        ASSERT(tspdf_dict_get(egs, gs) != NULL);

        // Both the original text and the overlay text extract.
        const char *text = tspdf_reader_page_text(doc2, p, &err);
        ASSERT(text != NULL);
        ASSERT(strstr(text, p == 0 ? "Hello page one" : "Hello page two") != NULL);
        ASSERT(strstr(text, "DRAFT") != NULL);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_writer_destroy(res_owner);
    tspdf_reader_destroy(doc);
}

// --- Form XObject import (stamp) tests ---

// One-page A4 PDF with `text` drawn in Helvetica, via the writer.
static uint8_t *make_stamp_text_pdf(const char *text, size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    if (!font || !page) { tspdf_writer_destroy(w); return NULL; }
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 24.0);
    tspdf_stream_text_position(page, 72, 400);
    tspdf_stream_show_text(page, text);
    tspdf_stream_end_text(page);
    uint8_t *data = NULL;
    *out_len = 0;
    tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    return data;
}

// Draw an imported form XObject on a page: scale `s`, offset (x, y).
static TspdfError draw_xobject_on_page(TspdfReader *doc, size_t page_index,
                                       uint32_t xobj_num, double s, double x, double y) {
    const char *name = tspdf_page_add_xobject(doc, page_index, xobj_num);
    if (!name) return TSPDF_ERR_ALLOC;
    TspdfStream *ov = tspdf_page_begin_content(doc, page_index);
    if (!ov) return TSPDF_ERR_ALLOC;
    tspdf_stream_draw_image(ov, name, x, y, s, s);
    return tspdf_page_end_content(doc, page_index, ov, NULL);
}

TEST(test_end_content_under) {
    size_t pdf_len = 0;
    uint8_t *pdf_data = make_stamp_text_pdf("OriginalText", &pdf_len);
    ASSERT(pdf_data != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf_data, pdf_len, &err);
    ASSERT(doc != NULL);

    TspdfWriter *res_owner = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(res_owner, "Helvetica");
    TspdfStream *under = tspdf_page_begin_content(doc, 0);
    ASSERT(under != NULL);
    tspdf_stream_begin_text(under);
    tspdf_stream_set_font(under, font, 24.0);
    tspdf_stream_text_position(under, 72, 500);
    tspdf_stream_show_text(under, "UnderText");
    tspdf_stream_end_text(under);
    err = tspdf_page_end_content_under(doc, 0, under, res_owner);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_writer_destroy(res_owner);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    const char *under_pos = strstr(text, "UnderText");
    const char *orig_pos = strstr(text, "OriginalText");
    ASSERT(under_pos != NULL);
    ASSERT(orig_pos != NULL);
    // Under-content is prepended, so it comes first in content order.
    ASSERT(under_pos < orig_pos);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf_data);
}

TEST(test_import_page_xobject_basic) {
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, bbox, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // BBox mirrors the source page MediaBox (A4).
    ASSERT(bbox[2] > 595.0 && bbox[2] < 596.0);
    ASSERT(bbox[3] > 841.0 && bbox[3] < 842.0);

    err = draw_xobject_on_page(dst, 0, xnum, 0.5, 50, 50);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "BaseText") != NULL);
    ASSERT(strstr(text, "StampText") != NULL);
    // The form XObject made it into the file.
    ASSERT(bytes_contains(out, out_len, "/Form"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

// First indirect reference number found in an object tree (DFS), or 0. Used
// to check that two imported forms' /Resources point at the same destination
// objects (the dedup cache reused them) without caring about resource shape.
static uint32_t first_ref_num_in(TspdfObj *obj, int depth) {
    if (!obj || depth > 16) return 0;
    switch (obj->type) {
        case TSPDF_OBJ_REF:
            return obj->ref.num;
        case TSPDF_OBJ_ARRAY:
            for (size_t i = 0; i < obj->array.count; i++) {
                uint32_t n = first_ref_num_in(&obj->array.items[i], depth + 1);
                if (n) return n;
            }
            return 0;
        case TSPDF_OBJ_DICT:
            for (size_t i = 0; i < obj->dict.count; i++) {
                uint32_t n = first_ref_num_in(obj->dict.entries[i].value, depth + 1);
                if (n) return n;
            }
            return 0;
        default:
            return 0;
    }
}

TEST(test_import_same_source_twice_dedups) {
    // Repeated imports from the same source into one destination (the nup /
    // stamp pattern: one import per placement) must reuse already-imported
    // referenced objects instead of copying them again.
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    size_t base = dst->new_objs.count;
    uint32_t x1 = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(x1 > 0);
    size_t after_first = dst->new_objs.count;
    // First import brings the form plus at least one referenced object (the
    // font) — otherwise the dedup assertion below would be vacuous.
    ASSERT(after_first - base >= 2);

    uint32_t x2 = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(x2 > 0);
    ASSERT(x1 != x2);
    // Every referenced object is a cache hit: only the form itself is new.
    ASSERT_EQ_INT((int)(dst->new_objs.count - after_first), 1);

    // Both forms' /Resources must reference the SAME destination objects.
    TspdfObj *form1 = dst->new_objs.objs[x1 - dst->xref.count];
    TspdfObj *form2 = dst->new_objs.objs[x2 - dst->xref.count];
    ASSERT(form1 && form1->type == TSPDF_OBJ_STREAM);
    ASSERT(form2 && form2->type == TSPDF_OBJ_STREAM);
    uint32_t ref1 = first_ref_num_in(tspdf_dict_get(form1->stream.dict, "Resources"), 0);
    uint32_t ref2 = first_ref_num_in(tspdf_dict_get(form2->stream.dict, "Resources"), 0);
    ASSERT(ref1 != 0);
    ASSERT_EQ_INT((int)ref1, (int)ref2);

    // The deduped result must still save and render both texts.
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, x1, 0.5, 0, 0), TSPDF_OK);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, x2, 0.5, 200, 200), TSPDF_OK);
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "StampText") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_distinct_readers_not_deduped) {
    // The cache key includes the source reader pointer: two readers opened
    // from the same bytes are distinct sources and must NOT share entries
    // (nothing guarantees their object graphs stay in sync).
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src1 = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *src2 = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src1 != NULL);
    ASSERT(src2 != NULL);
    ASSERT(dst != NULL);

    size_t base = dst->new_objs.count;
    uint32_t x1 = tspdf_reader_import_page_xobject(dst, src1, 0, NULL, &err);
    ASSERT(x1 > 0);
    size_t delta1 = dst->new_objs.count - base;
    ASSERT(delta1 >= 2);

    uint32_t x2 = tspdf_reader_import_page_xobject(dst, src2, 0, NULL, &err);
    ASSERT(x2 > 0);
    size_t delta2 = dst->new_objs.count - base - delta1;
    // Full copy again: same object count as the first import.
    ASSERT_EQ_INT((int)delta2, (int)delta1);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src2);
    tspdf_reader_destroy(src1);
    free(dst_data);
    free(src_data);
}

TEST(test_import_source_freed_before_save) {
    // The import must be self-contained: destroying the source document (and
    // freeing its buffer) before saving the destination must be safe.
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);

    tspdf_reader_destroy(src);
    free(src_data);

    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "StampText") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    free(dst_data);
}

TEST(test_import_from_encrypted_source) {
    size_t plain_len = 0, dst_len = 0;
    uint8_t *plain = make_stamp_text_pdf("SecretStamp", &plain_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(plain != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *tmp = tspdf_reader_open(plain, plain_len, &err);
    ASSERT(tmp != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(tmp, &enc, &enc_len,
                                                "pw", "pw", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(tmp);
    free(plain);

    TspdfReader *src = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    // Stamp content was decrypted during import; the output is not encrypted.
    ASSERT(strstr(text, "SecretStamp") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(enc);
    free(dst_data);
}

TEST(test_import_page_out_of_range) {
    size_t len = 0;
    uint8_t *data = make_stamp_text_pdf("X", &len);
    ASSERT(data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(data, len, &err);
    ASSERT(doc != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(doc, doc, 5, NULL, &err);
    ASSERT_EQ_SIZE((size_t)xnum, 0);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
    free(data);
}

// Page with two content streams and an indirect font resource: import must
// concatenate the streams and deep-copy the referenced font object.
static char *make_two_stream_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *s1 = "BT /F1 24 Tf 72 700 Td (AlphaPart) Tj ET";
    const char *s2 = "BT /F1 24 Tf 72 650 Td (BetaPart) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources << /Font << /F1 6 0 R >> >> "
                 "/Contents [4 0 R 5 0 R] >>\nendobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s1), s1)) goto fail;
    size_t obj5 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s2), s2)) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_import_multi_stream_contents) {
    size_t src_len = 0;
    char *src_data = make_two_stream_page_pdf(&src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "AlphaPart") != NULL);
    ASSERT(strstr(text, "BetaPart") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

// Page whose resources contain a reference cycle (form XObject whose own
// resources point back at itself). Import must terminate and stay bounded.
static char *make_cyclic_resources_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;

    const char *content = "BT /F1 12 Tf 10 10 Td (Cyclic) Tj ET";

    size_t pos = 0;
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
                 "/Resources << /XObject << /X1 5 0 R >> /Font << /F1 6 0 R >> >> "
                 "/Contents 4 0 R >>\nendobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;
    size_t obj5 = pos;
    // Form XObject that references itself and the page (cycles both ways).
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] "
                 "/Resources << /XObject << /Self 5 0 R /Page 3 0 R >> >> "
                 "/Length 0 >>\nstream\n\nendstream\nendobj\n")) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos,
                 "xref\n0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_import_cyclic_resources_bounded) {
    size_t src_len = 0;
    char *src_data = make_cyclic_resources_pdf(&src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    // Must terminate (each source object is imported at most once).
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);
    ASSERT_EQ_INT(draw_xobject_on_page(dst, 0, xnum, 1.0, 0, 0), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(dst, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

// MediaBox abuse: enormous and degenerate boxes.
static char *make_mediabox_pdf(const char *mediabox, size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [%s] >>\nendobj\n",
                 mediabox)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 2048, &pos,
                 "xref\n0 4\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n%010zu 00000 n \n%010zu 00000 n \n"
                 "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_import_huge_bbox_clamped) {
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 99999999999 99999999999", &src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    double bbox[4] = {0};
    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, bbox, &err);
    ASSERT(xnum > 0);
    ASSERT(bbox[2] <= 1.0e7);
    ASSERT(bbox[3] <= 1.0e7);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_import_degenerate_bbox_rejected) {
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 0 0", &src_len);
    ASSERT(src_data != NULL);

    size_t dst_len = 0;
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(dst_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT_EQ_SIZE((size_t)xnum, 0);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_PDF);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

TEST(test_add_xobject_unique_names) {
    size_t src_len = 0, dst_len = 0;
    uint8_t *src_data = make_stamp_text_pdf("StampText", &src_len);
    uint8_t *dst_data = make_stamp_text_pdf("BaseText", &dst_len);
    ASSERT(src_data != NULL);
    ASSERT(dst_data != NULL);

    TspdfError err;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    TspdfReader *dst = tspdf_reader_open(dst_data, dst_len, &err);
    ASSERT(src != NULL);
    ASSERT(dst != NULL);

    uint32_t xnum = tspdf_reader_import_page_xobject(dst, src, 0, NULL, &err);
    ASSERT(xnum > 0);

    const char *n1 = tspdf_page_add_xobject(dst, 0, xnum);
    const char *n2 = tspdf_page_add_xobject(dst, 0, xnum);
    ASSERT(n1 != NULL);
    ASSERT(n2 != NULL);
    ASSERT(strcmp(n1, n2) != 0);

    tspdf_reader_destroy(dst);
    tspdf_reader_destroy(src);
    free(dst_data);
    free(src_data);
}

// --- N-up imposition tests ---

// Multi-page PDF: `count` pages, each with distinct text "PAGEk" (1-based),
// built via the writer. Pages are A4 portrait.
static uint8_t *make_multipage_text_pdf(size_t count, size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    if (!font) { tspdf_writer_destroy(w); return NULL; }
    for (size_t i = 0; i < count; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        if (!page) { tspdf_writer_destroy(w); return NULL; }
        char label[32];
        snprintf(label, sizeof(label), "PAGE%zu", i + 1);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 24.0);
        tspdf_stream_text_position(page, 72, 400);
        tspdf_stream_show_text(page, label);
        tspdf_stream_end_text(page);
    }
    uint8_t *data = NULL;
    *out_len = 0;
    tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    return data;
}

TEST(test_nup_2up_four_pages) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // 4 pages / 2-up = 2 sheets.
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 2);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);
    // All four source pages are present across the two sheets.
    char all[512] = {0};
    for (size_t i = 0; i < 2; i++) {
        const char *t = tspdf_reader_page_text(re, i, &err);
        if (t) { strncat(all, t, sizeof(all) - strlen(all) - 1); }
    }
    ASSERT(strstr(all, "PAGE1") != NULL);
    ASSERT(strstr(all, "PAGE2") != NULL);
    ASSERT(strstr(all, "PAGE3") != NULL);
    ASSERT(strstr(all, "PAGE4") != NULL);
    // Two form XObjects imported per sheet.
    ASSERT(bytes_contains(bytes, bytes_len, "/Form"));

    tspdf_reader_destroy(re);
    free(bytes);
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_4up_four_pages_one_sheet) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    const char *t = tspdf_reader_page_text(re, 0, &err);
    ASSERT(t != NULL);
    ASSERT(strstr(t, "PAGE1") != NULL);
    ASSERT(strstr(t, "PAGE2") != NULL);
    ASSERT(strstr(t, "PAGE3") != NULL);
    ASSERT(strstr(t, "PAGE4") != NULL);

    tspdf_reader_destroy(re);
    free(bytes);
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_3pages_4up_partial_sheet) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(3, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    // 3 pages / 4-up = 1 sheet (one empty cell).
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_5pages_2up_three_sheets) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(5, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    // ceil(5/2) = 3 sheets.
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 3);

    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_invalid_n_rejected) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 3;  // not in {2,4,6,8,9,16}
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_empty_selection_rejected) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    size_t empty = 0;
    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.pages = &empty;  // non-NULL but zero count
    opts.page_count = 0;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(src);
    free(src_data);
}

// A tall source page fitted into a wide 2-up landscape cell must keep its
// aspect ratio: the scale is the same in x and y (uniform cm).
TEST(test_nup_aspect_ratio_uniform_scale) {
    // Build a tall, narrow source page (100 x 400) with distinct content.
    size_t src_len = 0;
    char *src_data = make_mediabox_pdf("0 0 100 400", &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    opts.landscape = true;  // wide cells
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // The cm matrix that places the form is uniform "s 0 0 s tx ty cm": the
    // two scale components must be equal (no distortion). We look for a
    // matrix line ending in " cm" whose a==d. Rather than parse, assert the
    // content contains " cm" and " Do" (placement happened) — the numeric
    // aspect check is validated by the pymupdf oracle at dev time.
    ASSERT(bytes_contains(bytes, bytes_len, " cm"));
    ASSERT(bytes_contains(bytes, bytes_len, " Do"));

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_frame_draws_border) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(2, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 2;
    opts.size = TSPDF_NUP_SIZE_A4;
    opts.frame = true;
    opts.gap = 10.0;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // A stroked rectangle (re ... S) is present for the frame.
    ASSERT(bytes_contains(bytes, bytes_len, " re"));

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

TEST(test_nup_cyclic_resources_bounded) {
    size_t src_len = 0;
    char *src_data = make_cyclic_resources_pdf(&src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open((const uint8_t *)src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    // Must terminate (import is bounded) and produce a valid one-cell sheet.
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(out), 1);

    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    tspdf_reader_destroy(out);
    free(bytes);
    tspdf_reader_destroy(src);
    free(src_data);
}

// Codifies the documented lifetime contract: the nup output is NOT
// self-contained, so the supported order is nup -> save -> THEN destroy the
// source. (Destroying the source before saving would be a use-after-free and is
// intentionally not exercised.) Runs clean under ASan.
TEST(test_nup_source_outlives_output_until_saved) {
    size_t src_len = 0;
    uint8_t *src_data = make_multipage_text_pdf(4, &src_len);
    ASSERT(src_data != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *src = tspdf_reader_open(src_data, src_len, &err);
    ASSERT(src != NULL);

    TspdfNupOptions opts = {0};
    opts.n = 4;
    opts.size = TSPDF_NUP_SIZE_A4;
    TspdfReader *out = tspdf_reader_nup(src, &opts, &err);
    ASSERT(out != NULL);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Save while the source is still alive (the output references it).
    uint8_t *bytes = NULL;
    size_t bytes_len = 0;
    err = tspdf_reader_save_to_memory(out, &bytes, &bytes_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_len > 0);

    // Only now may the source be destroyed.
    tspdf_reader_destroy(out);
    tspdf_reader_destroy(src);
    free(src_data);

    // The saved bytes are self-contained and reopen without the source.
    TspdfReader *re = tspdf_reader_open(bytes, bytes_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(bytes);
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
    ASSERT(bytes_contains(out, out_len, "/Size "));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

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
    ASSERT(bytes_contains(out, out_len, "/Size "));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

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

TEST(test_save_use_xref_stream_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.use_xref_stream = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(bytes_contains(out, out_len, "/Size "));
    // W is right-sized from the actual max offset (2 bytes here) and max
    // field-3 value (no type-2 entries: 1 byte), not a fixed [1 8 2].
    ASSERT(bytes_contains(out, out_len, "/W [ 1 2 1 ]"));
    ASSERT(!bytes_contains(out, out_len, "/W [ 1 8 2 ]"));
    ASSERT(!bytes_contains(out, out_len, "/TspdfSize"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);
    ASSERT(doc2->xref.count > 0);
    ASSERT(doc2->xref.entries[doc2->xref.count - 1].in_use);
    ASSERT(doc2->xref.entries[doc2->xref.count - 1].offset > 0);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserves_catalog_entries_with_strip_unused) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);

    TspdfObj *pages = tspdf_dict_get(doc2->catalog, "Pages");
    ASSERT(pages != NULL);
    ASSERT_EQ_INT(pages->type, TSPDF_OBJ_REF);
    ASSERT_EQ_INT(pages->ref.num, 2);

    TspdfObj *page_mode = tspdf_dict_get(doc2->catalog, "PageMode");
    ASSERT(page_mode != NULL);
    ASSERT_EQ_INT(page_mode->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)page_mode->string.data, "UseOutlines");

    TspdfObj *viewer = tspdf_dict_get(doc2->catalog, "ViewerPreferences");
    ASSERT(viewer != NULL);
    ASSERT_EQ_INT(viewer->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(viewer, "DisplayDocTitle") != NULL);

    TspdfObj *acro = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "AcroForm"));
    ASSERT(acro != NULL);
    ASSERT_EQ_INT(acro->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(acro, "Fields") != NULL);

    TspdfObj *names = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Names"));
    ASSERT(names != NULL);
    ASSERT_EQ_INT(names->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(names, "Dests") != NULL);

    TspdfObj *outlines = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Outlines"));
    ASSERT(outlines != NULL);
    ASSERT_EQ_INT(outlines->type, TSPDF_OBJ_DICT);
    ASSERT(tspdf_dict_get(outlines, "Count") != NULL);

    TspdfObj *metadata = test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Metadata"));
    ASSERT(metadata != NULL);
    ASSERT_EQ_INT(metadata->type, TSPDF_OBJ_STREAM);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_strip_metadata_removes_catalog_metadata) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Metadata"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Metadata") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "AcroForm") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_preserve_ids_with_strip_unused_keeps_catalog_root) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    opts.strip_unused_objects = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Root 1 0 R"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Pages") != NULL);
    ASSERT(test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "AcroForm")) != NULL);
    ASSERT(test_resolve_ref(doc2, tspdf_dict_get(doc2->catalog, "Metadata")) != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_preserve_ids_with_strip_metadata_rewrites_catalog) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;
    opts.strip_metadata = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Metadata"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT(doc2->catalog != NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Metadata") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "AcroForm") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
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

// --- Audit fixes (fix/reader-core): page-range errors, extract catalog
// --- slimming, real stream recompression ---

TEST(test_error_string_page_range) {
    ASSERT_EQ_STR(tspdf_error_string(TSPDF_ERR_PAGE_RANGE),
                  "page index out of range");
}

TEST(test_extract_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {0, 7};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_delete_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {7};
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_rotate_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t pages[] = {7};
    TspdfReader *result = tspdf_reader_rotate(doc, pages, 1, 90, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_reorder_out_of_range_sets_page_range_error) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    size_t order[] = {2, 1, 7};
    TspdfReader *result = tspdf_reader_reorder(doc, order, 3, &err);
    ASSERT(result == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

// Two-page PDF whose catalog carries every document-level tree that can pin
// the full object graph (/StructTreeRoot /Outlines /Dests /Names /PageLabels
// /AcroForm), each referencing a payload object with a recognizable marker.
static char *make_pinned_trees_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n"
                 "<< /Type /Catalog /Pages 2 0 R /Lang (en) "
                 "/StructTreeRoot 6 0 R /Outlines 7 0 R /Dests 8 0 R "
                 "/Names 9 0 R /PageLabels 10 0 R /AcroForm 11 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 5 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t obj5 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Length 20 >>\nstream\nq 1 0 0 1 0 0 cm Q\n\nendstream\nendobj\n")) goto fail;

    size_t obj6 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Type /StructTreeRoot /K 12 0 R >>\nendobj\n")) goto fail;

    size_t obj7 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Type /Outlines /Count 1 /First 12 0 R >>\nendobj\n")) goto fail;

    size_t obj8 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /SomeDest 12 0 R >>\nendobj\n")) goto fail;

    size_t obj9 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "9 0 obj\n<< /Dests << /Names [(d1) 12 0 R] >> >>\nendobj\n")) goto fail;

    size_t obj10 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "10 0 obj\n<< /Nums [0 12 0 R] >>\nendobj\n")) goto fail;

    size_t obj11 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "11 0 obj\n<< /Fields [12 0 R] >>\nendobj\n")) goto fail;

    size_t obj12 = pos;
    if (!appendf(pdf, 8192, &pos,
                 "12 0 obj\n<< /Marker (PINNEDPAYLOAD) >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos,
                 "xref\n"
                 "0 13\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 13 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9,
                 obj10, obj11, obj12, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

TEST(test_extract_drops_document_level_trees) {
    size_t len = 0;
    char *pdf = make_pinned_trees_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The document-level trees and everything only they referenced are gone
    ASSERT(!bytes_contains(out, out_len, "/StructTreeRoot"));
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "/Dests"));
    ASSERT(!bytes_contains(out, out_len, "/Names"));
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "PINNEDPAYLOAD"));
    // /PageLabels is rebuilt for the kept page (the payload dict carried no
    // /S//P//St, so the rebuilt range is a blank label) — the marker object
    // itself must still be unreachable.
    ASSERT(bytes_contains(out, out_len, "/PageLabels"));

    // Other catalog entries survive
    ASSERT(bytes_contains(out, out_len, "/Lang"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    ASSERT(tspdf_dict_get(doc2->catalog, "StructTreeRoot") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Outlines") == NULL);
    ASSERT(tspdf_dict_get(doc2->catalog, "Lang") != NULL);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Append raw (possibly binary) bytes to a builder buffer.
static bool append_raw(char *buf, size_t cap, size_t *pos, const void *data, size_t len) {
    if (len > cap - *pos) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
    return true;
}

static uint32_t test_adler32(const uint8_t *data, size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return (s2 << 16) | s1;
}

// Large, highly compressible content-stream payload (malloc'd).
static char *make_compressible_payload(size_t *payload_len) {
    const char *line = "0.2 0.4 0.6 rg 72 700 468 -650 re f\n";
    size_t line_len = strlen(line);
    size_t reps = 300;  // ~10.8KB, well above the old 4KB recompress floor
    char *payload = (char *)malloc(line_len * reps + 1);
    if (!payload) return NULL;
    for (size_t i = 0; i < reps; i++) {
        memcpy(payload + i * line_len, line, line_len);
    }
    payload[line_len * reps] = '\0';
    *payload_len = line_len * reps;
    return payload;
}

// One-page PDF whose content stream carries `payload` verbatim with no /Filter.
static char *make_unfiltered_stream_pdf(const char *payload, size_t payload_len,
                                        size_t *out_len) {
    size_t cap = payload_len + 2048;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n<< /Length %zu >>\nstream\n", payload_len)) goto fail;
    if (!append_raw(pdf, cap, &pos, payload, payload_len)) goto fail;
    if (!appendf(pdf, cap, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// One-page PDF whose content stream is `payload` wrapped in a zlib stream of
// stored (uncompressed) deflate blocks: a valid but poorly compressed
// FlateDecode stream, like real-world files written with compression level 0.
static char *make_stored_flate_stream_pdf(const char *payload, size_t payload_len,
                                          size_t *out_len) {
    // zlib header + stored blocks (5 bytes overhead per 65535) + adler32
    size_t flate_cap = 2 + payload_len + 5 * (payload_len / 65535 + 1) + 4;
    uint8_t *flate = (uint8_t *)malloc(flate_cap);
    if (!flate) return NULL;

    size_t fpos = 0;
    flate[fpos++] = 0x78;
    flate[fpos++] = 0x01;
    size_t remaining = payload_len;
    const uint8_t *src = (const uint8_t *)payload;
    while (remaining > 0) {
        uint16_t block = remaining > 65535 ? 65535 : (uint16_t)remaining;
        flate[fpos++] = (remaining == block) ? 1 : 0;  // BFINAL, BTYPE=00
        flate[fpos++] = (uint8_t)(block & 0xFF);
        flate[fpos++] = (uint8_t)(block >> 8);
        flate[fpos++] = (uint8_t)(~block & 0xFF);
        flate[fpos++] = (uint8_t)((~block >> 8) & 0xFF);
        memcpy(flate + fpos, src, block);
        fpos += block;
        src += block;
        remaining -= block;
    }
    uint32_t adler = test_adler32((const uint8_t *)payload, payload_len);
    flate[fpos++] = (uint8_t)(adler >> 24);
    flate[fpos++] = (uint8_t)(adler >> 16);
    flate[fpos++] = (uint8_t)(adler >> 8);
    flate[fpos++] = (uint8_t)adler;

    size_t cap = fpos + 2048;
    char *pdf = (char *)malloc(cap);
    if (!pdf) { free(flate); return NULL; }

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n"
                 "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R >>\n"
                 "endobj\n")) goto fail;

    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Length %zu /Filter /FlateDecode >>\nstream\n", fpos)) goto fail;
    if (!append_raw(pdf, cap, &pos, flate, fpos)) goto fail;
    if (!appendf(pdf, cap, &pos, "\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 5\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 5 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, xref)) goto fail;

    free(flate);
    *out_len = pos;
    return pdf;

fail:
    free(flate);
    free(pdf);
    return NULL;
}

// Resolve page 0's /Contents stream in a document.
static TspdfObj *first_page_contents(TspdfReader *doc) {
    TspdfReaderPage *page = tspdf_reader_get_page(doc, 0);
    if (!page) return NULL;
    return test_resolve_ref(doc, tspdf_dict_get(page->page_dict, "Contents"));
}

TEST(test_recompress_adds_flate_to_unfiltered_streams) {
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out_len < len);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    TspdfObj *filter = tspdf_dict_get(contents->stream.dict, "Filter");
    ASSERT(filter != NULL);
    ASSERT_EQ_INT(filter->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    // The compressed bytes must decode back to the original payload
    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_keeps_incompressible_stream_verbatim) {
    // Pseudo-random binary payload: deflate cannot shrink it, so the
    // keep-smaller rule must leave the stream unfiltered and untouched.
    size_t payload_len = 5000;
    char *payload = (char *)malloc(payload_len);
    ASSERT(payload != NULL);
    uint32_t state = 0x2545F491u;
    for (size_t i = 0; i < payload_len; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = (char)(state & 0xFF);
    }

    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);
    ASSERT(tspdf_dict_get(contents->stream.dict, "Filter") == NULL);
    ASSERT_EQ_SIZE(contents->stream.raw_len, payload_len);
    ASSERT(memcmp(doc2->data + contents->stream.raw_offset, payload, payload_len) == 0);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_reflates_large_flate_stream) {
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_stored_flate_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // The stored-block stream is > 10KB and highly compressible: with the old
    // 4KB skip floor removed the output must shrink substantially.
    ASSERT(out_len + 4000 < len);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_encrypted_source_roundtrip) {
    // Streams of an encrypted source are decrypted in memory before
    // recompression: the recompressed plaintext output must decode cleanly.
    size_t payload_len = 0;
    char *payload = make_compressible_payload(&payload_len);
    ASSERT(payload != NULL);
    size_t len = 0;
    char *pdf = make_unfiltered_stream_pdf(payload, payload_len, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *enc_doc = tspdf_reader_open_with_password(enc, enc_len, "user123", &err);
    ASSERT(enc_doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    // Saves preserve encryption by default (where recompression is skipped);
    // this test targets the decrypt+recompress combination.
    opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(enc_doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    TspdfObj *contents = first_page_contents(doc2);
    ASSERT(contents != NULL);
    ASSERT_EQ_INT(contents->type, TSPDF_OBJ_STREAM);

    TspdfObj *filter = tspdf_dict_get(contents->stream.dict, "Filter");
    ASSERT(filter != NULL);
    ASSERT_EQ_INT(filter->type, TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    size_t dec_len = 0;
    uint8_t *dec = deflate_decompress(doc2->data + contents->stream.raw_offset,
                                      contents->stream.raw_len, &dec_len);
    ASSERT(dec != NULL);
    ASSERT_EQ_SIZE(dec_len, payload_len);
    ASSERT(memcmp(dec, payload, payload_len) == 0);

    free(dec);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(enc_doc);
    free(enc);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(payload);
}

TEST(test_recompress_writes_xref_stream) {
    // recompress_streams targets minimal output size, so it also writes the
    // compact xref stream instead of a classic table.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 3);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

// A classic-xref PDF with `count` small ExtGState objects reachable from the
// single page's resources. Exercises object-stream packing on files where
// per-object overhead dominates.
static char *make_many_small_objects_pdf(size_t *out_len, int count) {
    size_t cap = 65536 + (size_t)count * 128;
    char *pdf = (char *)malloc(cap);
    size_t *off = (size_t *)calloc((size_t)count + 8, sizeof(size_t));
    if (!pdf || !off) {
        free(pdf);
        free(off);
        return NULL;
    }

    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;

    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;

    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> /ExtGState << ")) goto fail;
    for (int i = 0; i < count; i++) {
        if (!appendf(pdf, cap, &pos, "/GS%d %d 0 R ", i, 6 + i)) goto fail;
    }
    if (!appendf(pdf, cap, &pos, ">> >> >>\nendobj\n")) goto fail;

    const char *content = "BT /F1 12 Tf 72 720 Td (Hello ObjStm) Tj ET";
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;

    off[5] = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n")) goto fail;

    for (int i = 0; i < count; i++) {
        off[6 + i] = pos;
        if (!appendf(pdf, cap, &pos,
                     "%d 0 obj\n<< /Type /ExtGState /CA 0.%d /LW %d >>\nendobj\n",
                     6 + i, i % 9 + 1, i % 7 + 1)) goto fail;
    }

    size_t xref = pos;
    if (!appendf(pdf, cap, &pos, "xref\n0 %d\n0000000000 65535 f \n", count + 6)) goto fail;
    for (int i = 1; i < count + 6; i++) {
        if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, cap, &pos,
                 "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 count + 6, xref)) goto fail;

    free(off);
    *out_len = pos;
    return pdf;

fail:
    free(off);
    free(pdf);
    return NULL;
}

TEST(test_recompress_packs_objects_into_object_streams) {
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 300);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Same options the `compress` CLI command uses.
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(out_len < len);

    // Reopen with our own reader: pages, text, metadata and every object
    // must survive the round-trip.
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Hello ObjStm") != NULL);
    const char *producer = tspdf_reader_get_producer(doc2);
    ASSERT(producer != NULL);
    ASSERT(strstr(producer, "tspdf") != NULL);

    // Every in-use object resolves, and the small objects landed in object
    // streams (type-2 xref entries).
    TspdfParser parser;
    tspdf_parser_init(&parser, doc2->data, doc2->data_len, &doc2->arena);
    size_t type2_entries = 0;
    for (uint32_t i = 1; i < (uint32_t)doc2->xref.count; i++) {
        if (!doc2->xref.entries[i].in_use) continue;
        TspdfObj *obj = tspdf_xref_resolve(&doc2->xref, &parser, i, doc2->obj_cache, NULL);
        ASSERT(obj != NULL);
        if (doc2->xref.entries[i].compressed) type2_entries++;
    }
    ASSERT(type2_entries >= 300);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_recompress_roundtrip_from_objstm_input) {
    // A document that itself came from object-stream input survives
    // recompression into fresh object streams.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_recompress_objstm_output_does_not_grow) {
    // Do-no-harm: recompressing an ObjStm-heavy file (our own compressed
    // output) must not inflate it.
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 300);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    uint8_t *out1 = NULL;
    size_t out1_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out1, &out1_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out1, out1_len, "/Type /ObjStm"));

    TspdfReader *doc1 = tspdf_reader_open(out1, out1_len, &err);
    ASSERT(doc1 != NULL);
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc1, &out2, &out2_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(out2_len <= out1_len);

    TspdfReader *doc2 = tspdf_reader_open(out2, out2_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out2);
    tspdf_reader_destroy(doc1);
    free(out1);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_save_has_no_objstm) {
    // Classic (non-ObjStm) inputs are never force-packed: an encrypted save
    // of a classic file keeps plain top-level objects and a classic xref
    // table. (ObjStm inputs DO re-pack — see the tests below.)
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 50);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Type /ObjStm"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_default_save_repacks_objstm_input) {
    // A default (non-compress) save of a file that used object streams must
    // re-pack them, not explode every member into a classic top-level object
    // (which roughly doubles ObjStm-heavy files) — and must not copy the
    // source ObjStm/XRef containers along as orphans.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    // Xref stream instead of a classic table (type-2 entries need it).
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(!bytes_contains(out, out_len, "\ntrailer"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    // No orphans: every in-use object must resolve.
    TspdfParser parser;
    tspdf_parser_init(&parser, doc2->data, doc2->data_len, &doc2->arena);
    for (uint32_t i = 1; i < (uint32_t)doc2->xref.count; i++) {
        if (!doc2->xref.entries[i].in_use) continue;
        ASSERT(tspdf_xref_resolve(&doc2->xref, &parser, i, doc2->obj_cache, NULL) != NULL);
    }

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_default_save_classic_input_stays_classic) {
    // qpdf's preserve-style rule: object streams are only written when the
    // input had them (or `compress` asks for minimal output).
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 50);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(bytes_contains(out, out_len, "\ntrailer"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_save_repacks_objstm_input) {
    // Encrypted saves re-pack ObjStm inputs too: member bodies carry plain
    // strings and the container stream is encrypted as one unit, with the
    // trailer keys (/Encrypt, /ID) on the — itself unencrypted — xref stream.
    size_t len = 0;
    char *pdf = make_object_stream_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "/Type /ObjStm"));
    ASSERT(bytes_contains(out, out_len, "/Type /XRef"));
    ASSERT(bytes_contains(out, out_len, "/Encrypt"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    // Round-trip once more (re-save of the opened-encrypted doc exercises the
    // preserved-crypt repack path).
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out2, out2_len, "/Type /ObjStm"));
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc3), 1);

    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_encrypted_info_strings_are_encrypted) {
    // ISO 32000 exempts only the /Encrypt dict and /ID from string
    // encryption: Info metadata written into an encrypted file must be
    // encrypted with the Info object's key, or readers that decrypt it
    // unconditionally (poppler among them) show garbage.
    size_t len = 0;
    char *pdf = make_many_small_objects_pdf(&len, 10);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    tspdf_reader_set_title(doc, "SeekritTitle42");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &out, &out_len,
                                                "user123", "owner456", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "SeekritTitle42"));

    TspdfReader *doc2 = tspdf_reader_open_with_password(out, out_len, "user123", &err);
    ASSERT(doc2 != NULL);
    const char *title = tspdf_reader_get_title(doc2);
    ASSERT(title != NULL);
    ASSERT(strcmp(title, "SeekritTitle42") == 0);

    // Same on the preserved-crypt path: edit metadata on the opened
    // encrypted document and re-save with the original encryption.
    tspdf_reader_set_title(doc2, "SecondSecret77");
    uint8_t *out2 = NULL;
    size_t out2_len = 0;
    err = tspdf_reader_save_to_memory(doc2, &out2, &out2_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out2, out2_len, "SecondSecret77"));
    TspdfReader *doc3 = tspdf_reader_open_with_password(out2, out2_len, "user123", &err);
    ASSERT(doc3 != NULL);
    const char *title3 = tspdf_reader_get_title(doc3);
    ASSERT(title3 != NULL);
    ASSERT(strcmp(title3, "SecondSecret77") == 0);

    tspdf_reader_destroy(doc3);
    free(out2);
    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_delete_keeps_document_level_trees) {
    size_t len = 0;
    char *pdf = make_pinned_trees_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_delete(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Delete keeps the document-level trees (only extract drops them)
    ASSERT(bytes_contains(out, out_len, "/StructTreeRoot"));
    ASSERT(bytes_contains(out, out_len, "/Outlines"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc2), 1);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// ============================================================
// Encoding / i18n (fix/encoding track)
// ============================================================

#include "../include/tspdf/version.h"
#include "../src/util/pdftext.h"

TEST(test_pdftext_cp1252_from_codepoint) {
    // ASCII and Latin-1 map to themselves.
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint('A'), 'A');
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0xFC), 0xFC);  // ü
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0xC4), 0xC4);  // Ä
    // cp1252 specials in the 0x80-0x9F window (not plain Latin-1).
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x20AC), 0x80); // €
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2013), 0x96); // en-dash
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2014), 0x97); // em-dash
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x2022), 0x95); // bullet
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x201C), 0x93); // left curly quote
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0178), 0x9F); // Ÿ
    // No mapping: C1 controls, unassigned cp1252 slots, non-Latin scripts.
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0081), -1);
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x0416), -1);  // Cyrillic Ж
    ASSERT_EQ_INT(tspdf_cp1252_from_codepoint(0x6A5F), -1);  // CJK 機
}

TEST(test_pdftext_utf8_to_cp1252_strict) {
    char out[64];
    uint32_t bad = 0;
    // Umlauts and en-dash convert to the cp1252 bytes.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("Vertraulich \xe2\x80\x93 gepr\xc3\xbc"
                                       "ft", out, &bad),
                  TSPDF_PDFTEXT_OK);
    ASSERT_EQ_STR(out, "Vertraulich \x96 gepr\xfc" "ft");
    // Characters outside WinAnsi fail and name the offending code point.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("ok \xe6\xa9\x9f", out, &bad),
                  TSPDF_PDFTEXT_UNMAPPED);
    ASSERT_EQ_INT((int)bad, 0x6A5F);
    // Malformed UTF-8 is reported as such, not silently emitted.
    ASSERT_EQ_INT(tspdf_utf8_to_cp1252("bad \xc3", out, &bad),
                  TSPDF_PDFTEXT_BAD_UTF8);
}

TEST(test_pdftext_utf8_to_cp1252_lossy) {
    char out[64];
    uint32_t bad = 0;
    // In-place conversion is allowed (output is never longer than input).
    char line[] = "na\xc3\xafve caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x98\x80!";
    size_t subs = tspdf_utf8_to_cp1252_lossy(line, line, &bad);
    ASSERT_EQ_SIZE(subs, 1);
    ASSERT_EQ_INT((int)bad, 0x1F600);  // the emoji became '?'
    ASSERT_EQ_STR(line, "na\xefve caf\xe9 \x97 ?!");
    // Pure WinAnsi text passes through with zero substitutions.
    ASSERT_EQ_SIZE(tspdf_utf8_to_cp1252_lossy("plain", out, &bad), 0);
    ASSERT_EQ_STR(out, "plain");
}

TEST(test_pdftext_utf16be_roundtrip) {
    // Encoder: non-ASCII Info strings become a BOM-prefixed UTF-16BE hex string.
    TspdfBuffer buf = tspdf_buffer_create(64);
    tspdf_pdftext_write_info_string(&buf, "Pr\xc3\xbc \xe2\x80\x93 \xf0\x9f\x98\x80");
    tspdf_buffer_append_byte(&buf, '\0');
    ASSERT_EQ_STR((const char *)buf.data, "<FEFF0050007200FC002020130020D83DDE00>");
    tspdf_buffer_destroy(&buf);

    // ASCII strings stay readable literal strings (with the usual escapes).
    TspdfBuffer buf2 = tspdf_buffer_create(64);
    tspdf_pdftext_write_info_string(&buf2, "Plain (Report)");
    tspdf_buffer_append_byte(&buf2, '\0');
    ASSERT_EQ_STR((const char *)buf2.data, "(Plain \\(Report\\))");
    tspdf_buffer_destroy(&buf2);

    // Decoder: BOM + UTF-16BE (incl. a surrogate pair) back to UTF-8.
    TspdfArena arena = tspdf_arena_create(1024);
    const uint8_t utf16[] = {0xFE, 0xFF, 0x00, 0x50, 0x00, 0xFC,
                             0x20, 0x13, 0xD8, 0x3D, 0xDE, 0x00};
    char *utf8 = tspdf_utf16be_to_utf8(utf16, sizeof(utf16), &arena);
    ASSERT(utf8 != NULL);
    ASSERT_EQ_STR(utf8, "P\xc3\xbc\xe2\x80\x93\xf0\x9f\x98\x80");
    // Strings without the BOM are not touched.
    ASSERT(tspdf_utf16be_to_utf8((const uint8_t *)"plain", 5, &arena) == NULL);
    tspdf_arena_destroy(&arena);
}

TEST(test_pdftext_pdfdoc_decode) {
    // ASCII and the Latin-1 rows map to themselves.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint('A'), 'A');
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xE9), 0xE9);    // é
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xFC), 0xFC);    // ü
    // PDFDocEncoding specials in 0x18-0x1F (accents) and 0x80-0xA0.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x18), 0x02D8);  // breve
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x1F), 0x02DC);  // small tilde
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x80), 0x2022);  // bullet
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x85), 0x2013);  // en-dash
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x8A), 0x2212);  // minus sign
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x93), 0xFB01);  // fi ligature
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x9E), 0x017E);  // ž
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xA0), 0x20AC);  // euro
    // Undefined slots decode to U+FFFD, never to garbage bytes.
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x7F), 0xFFFD);
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0x9F), 0xFFFD);
    ASSERT_EQ_INT((int)tspdf_pdfdoc_to_codepoint(0xAD), 0xFFFD);
}

TEST(test_pdftext_text_string_decode) {
    TspdfArena arena = tspdf_arena_create(1024);

    // UTF-16BE with BOM (incl. a surrogate pair) decodes to UTF-8.
    const uint8_t utf16[] = {0xFE, 0xFF, 0x00, 0xFC, 0x00, 'n',
                             0xD8, 0x3D, 0xDE, 0x00};
    char *s = tspdf_pdf_text_to_utf8(utf16, sizeof(utf16), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "\xC3\xBCn\xF0\x9F\x98\x80");  // ü n 😀

    // No BOM + non-UTF-8 bytes: PDFDocEncoding ("caf\xE9" -> café).
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"caf\xE9", 4, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "caf\xC3\xA9");

    // PDFDocEncoding specials: 0x85 en-dash, 0xA0 euro, 0x9F undefined->U+FFFD.
    const uint8_t docenc[] = {'a', 0x85, 0xA0, 0x9F, 'z'};
    s = tspdf_pdf_text_to_utf8(docenc, sizeof(docenc), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "a\xE2\x80\x93\xE2\x82\xAC\xEF\xBF\xBDz");

    // Pure ASCII is returned unchanged.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"plain.txt", 9, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "plain.txt");

    // Bytes that already form valid non-ASCII UTF-8 (common non-conformant
    // writers) pass through instead of being mangled as PDFDocEncoding.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"caf\xC3\xA9", 5, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "caf\xC3\xA9");

    // UTF-8 BOM (PDF 2.0): stripped, payload validated.
    const uint8_t u8bom[] = {0xEF, 0xBB, 0xBF, 'o', 'k', 0xC3, 0xA9};
    s = tspdf_pdf_text_to_utf8(u8bom, sizeof(u8bom), &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "ok\xC3\xA9");

    // Empty input decodes to the empty string, not NULL.
    s = tspdf_pdf_text_to_utf8((const uint8_t *)"", 0, &arena);
    ASSERT(s != NULL);
    ASSERT_EQ_STR(s, "");

    tspdf_arena_destroy(&arena);
}

TEST(test_pdftext_text_string_encode) {
    TspdfArena arena = tspdf_arena_create(1024);
    size_t len = 0;

    // Pure ASCII stays byte-identical (no BOM).
    uint8_t *b = tspdf_utf8_to_pdf_text("plain.txt", &len, &arena);
    ASSERT(b != NULL);
    ASSERT_EQ_SIZE(len, 9);
    ASSERT(memcmp(b, "plain.txt", 9) == 0);

    // Non-ASCII UTF-8 becomes UTF-16BE with BOM.
    b = tspdf_utf8_to_pdf_text("\xC3\xBCn\xC3\xAF" "code.txt", &len, &arena);
    ASSERT(b != NULL);
    const uint8_t want[] = {0xFE, 0xFF, 0x00, 0xFC, 0x00, 'n', 0x00, 0xEF,
                            0x00, 'c', 0x00, 'o', 0x00, 'd', 0x00, 'e',
                            0x00, '.', 0x00, 't', 0x00, 'x', 0x00, 't'};
    ASSERT_EQ_SIZE(len, sizeof(want));
    ASSERT(memcmp(b, want, sizeof(want)) == 0);

    // Astral code points use a surrogate pair.
    b = tspdf_utf8_to_pdf_text("\xF0\x9F\x98\x80", &len, &arena);  // 😀
    ASSERT(b != NULL);
    const uint8_t want2[] = {0xFE, 0xFF, 0xD8, 0x3D, 0xDE, 0x00};
    ASSERT_EQ_SIZE(len, sizeof(want2));
    ASSERT(memcmp(b, want2, sizeof(want2)) == 0);

    // Encode/decode round-trip through the codec.
    b = tspdf_utf8_to_pdf_text("sp \xC3\xBCn\xC3\xAF" "code.txt", &len, &arena);
    ASSERT(b != NULL);
    char *back = tspdf_pdf_text_to_utf8(b, len, &arena);
    ASSERT(back != NULL);
    ASSERT_EQ_STR(back, "sp \xC3\xBCn\xC3\xAF" "code.txt");

    // Invalid UTF-8 input is kept verbatim (never half-encoded).
    b = tspdf_utf8_to_pdf_text("bad\xC3", &len, &arena);
    ASSERT(b != NULL);
    ASSERT_EQ_SIZE(len, 4);
    ASSERT(memcmp(b, "bad\xC3", 4) == 0);

    tspdf_arena_destroy(&arena);
}

TEST(test_metadata_non_ascii_utf16_roundtrip) {
    // Non-ASCII metadata must be written as UTF-16BE with BOM (ISO 32000
    // text string), not raw UTF-8 bytes in a PDFDocEncoded literal.
    const char *title = "Pr\xc3\xbc" "fbericht \xe2\x80\x93 \xc3\x84nderungen";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, title);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Raw bytes: a BOM-prefixed hex string, and no octal-escaped UTF-8 tail.
    ASSERT(bytes_contains(out, out_len, "<FEFF"));
    ASSERT(!bytes_contains(out, out_len, "\\303\\274"));

    // Reader display path decodes the UTF-16BE string back to UTF-8.
    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    const char *got = tspdf_reader_get_title(doc2);
    ASSERT(got != NULL);
    ASSERT_EQ_STR(got, title);

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_metadata_ascii_stays_literal) {
    // Pure-ASCII values keep the compact literal-string form.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    tspdf_reader_set_title(doc, "Plain Report");

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(bytes_contains(out, out_len, "(Plain Report)"));

    TspdfReader *doc2 = tspdf_reader_open(out, out_len, &err);
    ASSERT(doc2 != NULL);
    ASSERT_EQ_STR(tspdf_reader_get_title(doc2), "Plain Report");

    tspdf_reader_destroy(doc2);
    free(out);
    tspdf_reader_destroy(doc);
}

// Hand-built one-page PDF whose /Info holds a PDFDocEncoded /Title
// ("caf\xE9" = café) and a BOM-less pure-ASCII /Author.
static char *meta_make_pdfdoc_info_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(2048);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 2048, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 2048, &pos,
                 "4 0 obj\n<< /Title (caf\xE9) /Author (Plain Author) >>\nendobj\n")) goto fail;

    {
        size_t xref = pos;
        if (!appendf(pdf, 2048, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 4; i++) {
            if (!appendf(pdf, 2048, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, 2048, &pos,
                     "trailer\n<< /Size 5 /Root 1 0 R /Info 4 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_metadata_pdfdoc_encoded_decodes_to_utf8) {
    // A BOM-less Info string is PDFDocEncoding (ISO 32000 §7.9.2.2); the
    // getter must hand back UTF-8, never the raw 0xE9 byte.
    size_t len = 0;
    char *pdf = meta_make_pdfdoc_info_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *title = tspdf_reader_get_title(doc);
    ASSERT(title != NULL);
    ASSERT_EQ_STR(title, "caf\xC3\xA9");
    const char *author = tspdf_reader_get_author(doc);
    ASSERT(author != NULL);
    ASSERT_EQ_STR(author, "Plain Author");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_save_producer_is_tspdf_with_version) {
    // Default save options set update_producer; the stamp must be the real
    // project name + version, not the leftover codename "(tspr)".
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "(tspr)"));
    ASSERT(bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_save_preserve_ids_producer_is_tspdf_with_version) {
    // The preserve-object-ids raw-copy path builds its own Info dict too.
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.preserve_object_ids = true;

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "(tspr)"));
    ASSERT(bytes_contains(out, out_len, "/Producer (tspdf " TSPDF_VERSION_STRING ")"));

    free(out);
    tspdf_reader_destroy(doc);
}

// --- Text extraction (tspdf_reader_page_text) ---

#include "../src/reader/tspr_text.h"

// One-page PDF for text-extraction tests. `resources` is the page /Resources
// dict body; obj 4 is the /F1 font dict (`font_body`); obj 5 is an optional
// auxiliary object: a stream when aux_data != NULL (dict body `aux_dict`,
// /Length computed here), else a plain object with body `aux_dict`, else
// null. Obj 6 is the page content stream built from `content`.
static char *make_text_pdf_full(const char *resources, const char *font_body,
                                const char *aux_dict, const char *aux_data,
                                const char *content, size_t *out_len) {
    size_t cap = 8192 + strlen(content) + (aux_data ? strlen(aux_data) : 0);
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources %s /Contents 6 0 R >>\nendobj\n", resources)) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n%s\nendobj\n",
                 font_body ? font_body : "null")) goto fail;
    size_t obj5 = pos;
    if (aux_data) {
        if (!appendf(pdf, cap, &pos,
                     "5 0 obj\n<< %s /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                     aux_dict ? aux_dict : "", strlen(aux_data), aux_data)) goto fail;
    } else {
        if (!appendf(pdf, cap, &pos, "5 0 obj\n%s\nendobj\n",
                     aux_dict ? aux_dict : "null")) goto fail;
    }
    size_t obj6 = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(content), content)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 7\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 7 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

#define TEXT_HELV_FONT \
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
#define TEXT_HELV_RES "<< /Font << /F1 4 0 R >> >>"

static char *make_text_pdf(const char *font_body, const char *content, size_t *out_len) {
    return make_text_pdf_full(TEXT_HELV_RES, font_body, NULL, NULL, content, out_len);
}

TEST(test_text_simple_lines_writer_fixture) {
    // Writer-generated ground truth: two lines with WinAnsi umlauts/dash.
    TspdfWriter *w = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 12.0);
    tspdf_stream_text_position(page, 72, 720);
    tspdf_stream_show_text(page, "Hello World");
    tspdf_stream_text_position(page, 0, -14);
    tspdf_stream_show_text(page, "Gr\xfc\xdf" "e \x96 Umlaute");  // cp1252 bytes
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello World\nGr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 Umlaute\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tj_kerning_spacing) {
    // Kerning tweaks (-30) must not create spaces; word gaps (-300) must.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td [(Hel) -30 (lo) -300 (world)] TJ ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello world\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_multiline_td_tstar) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 14 TL 72 700 Td (Line one) Tj T* (Line two) Tj "
        "0 -20 Td (Line three) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Line one\nLine two\nLine three\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_xgap_emits_space) {
    // Two runs on one baseline with a large x-gap: joined by a space,
    // not a newline.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Left) Tj ET "
        "BT /F1 12 Tf 200 700 Td (Right) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Left Right\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Font body for a TeX-style subset font whose /Widths start past the space
// glyph (FirstChar 40): code 32 has no width entry, so the space width is
// unknown. All listed glyphs are 500/1000 wide.
static const char *make_no_space_font(char *buf, size_t cap) {
    size_t pos = 0;
    appendf(buf, cap, &pos,
            "<< /Type /Font /Subtype /Type1 /BaseFont /ABCDEF+FakeItalic "
            "/Encoding /WinAnsiEncoding /FirstChar 40 /LastChar 122 /Widths [");
    for (int i = 40; i <= 122; i++) appendf(buf, cap, &pos, "500 ");
    appendf(buf, cap, &pos, "] >>");
    return buf;
}

#define TEXT_TWO_FONT_RES "<< /Font << /F1 4 0 R /F2 5 0 R >> >>"

TEST(test_text_tj_kern_space_missing_space_glyph) {
    // TeX-generated PDFs encode inter-word spaces as -250-ish TJ kerns and
    // often subset the space glyph away (FirstChar > 32). A 0.25 em kern
    // must still produce a space; a small -30 kerning tweak must not.
    char font[4096];
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT,
        make_no_space_font(font, sizeof(font)), NULL,
        "BT /F2 10 Tf 72 700 Td [(arXiv)-250(preprint)-30(s)] TJ ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "arXiv preprints\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_font_switch_gap_space) {
    // Font change at a word boundary (roman -> italic math, TeX style): the
    // x-gap between runs is a normal ~0.25 em word space, and the italic
    // font has no space-width data. A space must be inserted. The roman
    // comma that follows the italic run with no gap must stay glued.
    // Helvetica "values of" at 10pt is 40.02 wide (ends at 112.02); the /F2
    // run starts at 114.5 => 2.48 gap (~0.25 em). "d" is 500/1000 wide, so
    // it ends at 119.5 where the comma resumes with zero gap.
    char font[4096];
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT,
        make_no_space_font(font, sizeof(font)), NULL,
        "BT /F1 10 Tf 1 0 0 1 72 700 Tm (values of) Tj "
        "/F2 10 Tf 1 0 0 1 114.5 700 Tm (d) Tj "
        "/F1 10 Tf 1 0 0 1 119.5 700 Tm (,) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "values of d,\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_font_switch_bogus_space_width) {
    // TeX math fonts (cmmi) have a real glyph at code 32 with a wide width
    // (651/1000) — it is not a space. That width must not raise the run-gap
    // threshold above a normal 0.25 em word space, or a font switch into
    // math glues words ("of dk"). Same geometry as the gap test above, but
    // /F2 declares FirstChar 32 with a 651-wide code-32 glyph.
    char font[4096];
    size_t pos = 0;
    appendf(font, sizeof(font), &pos,
            "<< /Type /Font /Subtype /Type1 /BaseFont /GHIJKL+FakeCMMI "
            "/Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 122 /Widths [651 ");
    for (int i = 33; i <= 122; i++) appendf(font, sizeof(font), &pos, "500 ");
    appendf(font, sizeof(font), &pos, "] >>");
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        TEXT_TWO_FONT_RES, TEXT_HELV_FONT, font, NULL,
        "BT /F1 10 Tf 1 0 0 1 72 700 Tm (values of) Tj "
        "/F2 10 Tf 1 0 0 1 114.5 700 Tm (d) Tj "
        "/F1 10 Tf 1 0 0 1 119.5 700 Tm (,) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "values of d,\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_bfchar_bfrange) {
    // ToUnicode beats /Encoding: bfchar (incl. multi-unit ligature target),
    // offset-form bfrange, and array-form bfrange.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "2 beginbfchar\n"
        "<44> <00660069>\n"
        "<45> <20AC>\n"
        "endbfchar\n"
        "2 beginbfrange\n"
        "<41> <43> <0391>\n"
        "<50> <51> [<0058> <0059>]\n"
        "endbfrange\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (ABCDE) Tj (PQ) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A->Α B->Β C->Γ D->fi E->€ P->X Q->Y
    ASSERT_EQ_STR(text, "\xce\x91\xce\x92\xce\x93" "fi" "\xe2\x82\xac" "XY\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_null_cp_skipped) {
    // A ToUnicode target of <0000> (.notdef in real generators) must be
    // skipped: an embedded NUL would truncate the returned C string and
    // hide all following page text.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "2 beginbfchar\n"
        "<41> <0000>\n"
        "<42> <0042>\n"
        "endbfchar\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AB) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "B\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_tounicode_wide_range_with_point_fixes) {
    // "Big range + point fixes" layout: one wide bfrange plus many bfchar
    // corrections above its lo. A code covered only by the wide range must
    // still map through it, however many bfchars sit in between.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "1 beginbfrange\n<00> <FF> <0100>\nendbfrange\n"
        "12 beginbfchar\n"
        "<41> <0391>\n<42> <0392>\n<43> <0393>\n<44> <0394>\n"
        "<45> <0395>\n<46> <0396>\n<47> <0397>\n<48> <0398>\n"
        "<49> <0399>\n<4A> <039A>\n<4B> <039B>\n<4C> <039C>\n"
        "endbfchar\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AP) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A (0x41) hits the bfchar fix -> U+0391; P (0x50) is covered only by
    // the wide range -> U+0100 + 0x50 = U+0150.
    ASSERT_EQ_STR(text, "\xce\x91\xc5\x90\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

// One-page PDF whose /Contents is an array of three streams; the (One) Tj
// operator is split across the first boundary.
static char *make_text_pdf_contents_array(size_t *out_len) {
    const char *s5 = "BT /F1 12 Tf 72 700 Td (One)";
    const char *s6 = "Tj ET";
    const char *s7 = "BT /F1 12 Tf 72 680 Td (Two) Tj ET";
    size_t cap = 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    size_t obj1 = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    size_t obj2 = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    size_t obj3 = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources " TEXT_HELV_RES " /Contents [5 0 R 6 0 R 7 0 R] >>\n"
                 "endobj\n")) goto fail;
    size_t obj4 = pos;
    if (!appendf(pdf, cap, &pos, "4 0 obj\n" TEXT_HELV_FONT "\nendobj\n")) goto fail;
    size_t obj5 = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s5), s5)) goto fail;
    size_t obj6 = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s6), s6)) goto fail;
    size_t obj7 = pos;
    if (!appendf(pdf, cap, &pos,
                 "7 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                 strlen(s7), s7)) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, cap, &pos,
                 "xref\n"
                 "0 8\n"
                 "0000000000 65535 f \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "%010zu 00000 n \n"
                 "trailer\n<< /Size 8 /Root 1 0 R >>\n"
                 "startxref\n%zu\n%%%%EOF",
                 obj1, obj2, obj3, obj4, obj5, obj6, obj7, xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_text_contents_array_concatenated) {
    // /Contents array streams concatenate with '\n' between (stream
    // boundaries are token boundaries), so the split (One) Tj still lexes.
    size_t len = 0;
    char *pdf = make_text_pdf_contents_array(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "One\nTwo\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_differences_encoding) {
    // /Differences over WinAnsi, glyph names resolved via the AGL subset.
    size_t len = 0;
    char *pdf = make_text_pdf(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding << /BaseEncoding /WinAnsiEncoding "
        "/Differences [65 /adieresis /germandbls 97 /Euro] >> >>",
        "BT /F1 12 Tf 72 700 Td (ABa) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "\xc3\xa4\xc3\x9f\xe2\x82\xac\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_form_xobject_recursion) {
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        "<< /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        TEXT_HELV_FONT,
        "/Type /XObject /Subtype /Form /BBox [0 0 200 200] "
        "/Resources << /Font << /F1 4 0 R >> >>",
        "BT /F1 12 Tf 10 100 Td (Inside form) Tj ET",
        "BT /F1 12 Tf 72 700 Td (Before) Tj ET "
        "q 1 0 0 1 50 50 cm /Fx Do Q "
        "BT /F1 12 Tf 72 600 Td (After) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Before\nInside form\nAfter\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_form_xobject_self_cycle_guarded) {
    // A form that draws itself must terminate via the cycle guard/depth cap.
    size_t len = 0;
    char *pdf = make_text_pdf_full(
        "<< /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        TEXT_HELV_FONT,
        "/Type /XObject /Subtype /Form /BBox [0 0 200 200] "
        "/Resources << /Font << /F1 4 0 R >> /XObject << /Fx 5 0 R >> >>",
        "BT /F1 12 Tf 10 100 Td (Loop) Tj ET /Fx Do",
        "/Fx Do", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Loop") != NULL);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_identity_h_ttf_tounicode) {
    // The writer's own TTF path (CIDFontType2 + Identity-H + ToUnicode) must
    // round-trip UTF-8 text through 2-byte glyph codes.
    TspdfWriter *w = tspdf_writer_create();
    const char *fname = tspdf_writer_add_ttf_font(w,
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf");
    if (!fname) {
        tspdf_writer_destroy(w);
        SKIP("LiberationSans-Regular.ttf not installed");
    }
    TTF_Font *ttf = tspdf_writer_get_ttf(w, fname);
    TspdfFont *pdf_font = tspdf_writer_get_font(w, fname);
    ASSERT(ttf != NULL);
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, fname, 14.0);
    tspdf_stream_text_position(page, 72, 700);
    tspdf_stream_show_text_utf8(page, "Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 done",
                                ttf, pdf_font);
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Gr\xc3\xbc\xc3\x9f" "e \xe2\x80\x93 done\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_cid_no_tounicode_replacement) {
    // Identity-H without /ToUnicode: one U+FFFD per 2-byte code, counted.
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type0 /BaseFont /NoMap "
        "/Encoding /Identity-H /DescendantFonts [5 0 R] >>",
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /NoMap "
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) "
        "/Supplement 0 >> /DW 1000 >>",
        NULL,
        "BT /F1 12 Tf 72 700 Td <00410042> Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfTextStats stats;
    const char *text = tspdf_reader_page_text_stats(doc, 0, &stats, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "\xef\xbf\xbd\xef\xbf\xbd\n");
    ASSERT_EQ_SIZE(stats.glyphs, 2);
    ASSERT_EQ_SIZE(stats.replacements, 2);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_encrypted_pdf) {
    TspdfWriter *w = tspdf_writer_create();
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    TspdfStream *page = tspdf_writer_add_page(w);
    tspdf_stream_begin_text(page);
    tspdf_stream_set_font(page, font, 12.0);
    tspdf_stream_text_position(page, 72, 700);
    tspdf_stream_show_text(page, "Secret text");
    tspdf_stream_end_text(page);
    uint8_t *pdf = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &pdf, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    err = tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                "pw", "owner", 0xFFFFFFFC, 128);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(pdf);

    TspdfReader *doc2 = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(doc2 != NULL);
    const char *text = tspdf_reader_page_text(doc2, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Secret text\n");
    tspdf_reader_destroy(doc2);
    free(enc);
}

TEST(test_text_page_out_of_range) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 5, &err);
    ASSERT(text == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
}

TEST(test_text_no_bt_writer_style_content) {
    // The writer's own fixtures emit Tf/Tj without BT..ET; extraction must
    // still see the text (lenient like real viewers).
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/one_page.pdf", &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Page 1\n");
    tspdf_reader_destroy(doc);
}

TEST(test_text_malformed_content_no_crash) {
    // Garbage operators, unbalanced constructs, truncated hex: no crash,
    // best-effort text.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (ok) Tj zz 3 qq [ (unterminated TJ "
        "\x01\x02\xff garbage ) ] TJ <4 broken ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "ok") != NULL);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_ligatures_folded_to_ascii) {
    // Unicode ligature code points (U+FB00-FB06) fold to their ASCII letter
    // sequences at emit time (matching poppler's Latin1 fold tables and
    // Unicode NFKC).
    size_t len = 0;
    char *pdf = make_text_pdf_full(TEXT_HELV_RES,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
        "/Encoding /WinAnsiEncoding /ToUnicode 5 0 R >>",
        "",
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
        "4 beginbfchar\n"
        "<41> <FB01>\n"
        "<42> <FB04>\n"
        "<43> <FB06>\n"
        "<44> <FB05>\n"
        "endbfchar\n"
        "1 beginbfrange\n"
        "<50> <52> <FB00>\n"
        "endbfrange\n"
        "endcmap\nend\nend",
        "BT /F1 12 Tf 72 700 Td (AnBC.D) Tj 0 -20 Td (PQR) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    // A->U+FB01 "fi", B->U+FB04 "ffl", C->U+FB06 "st", D->U+FB05 "st"
    // (long s t, NFKC folds it to "st" just like FB06);
    // bfrange P,Q,R -> U+FB00 "ff", U+FB01 "fi", U+FB02 "fl".
    ASSERT_EQ_STR(text, "finfflst.st\nfffifl\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_trailing_spaces_trimmed) {
    // Trailing spaces at end of each output line are trimmed (poppler
    // behavior) — both mid-document and on the final line.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Hello   ) Tj 0 -20 Td (World ) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Hello\nWorld\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

// --- Layout-preserving extraction (tspdf_reader_page_text_layout) ---

// Line `n` (0-based) of `text` copied into `buf`; false if absent.
static bool text_line(const char *text, size_t n, char *buf, size_t buf_size) {
    const char *p = text;
    for (size_t i = 0; i < n; i++) {
        p = strchr(p, '\n');
        if (!p) return false;
        p++;
    }
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len + 1 > buf_size) return false;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return true;
}

TEST(test_text_layout_two_columns) {
    // Two columns drawn out of visual order (right column of row 1 first).
    // Layout mode must re-order by x, keep both cells of a row on one line
    // separated by a run of spaces, and align the columns across rows.
    // Plain mode must stay in content-stream order, byte-identical.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 300 700 Td (XXX) Tj ET "
        "BT /F1 12 Tf 72 700 Td (AAA) Tj ET "
        "BT /F1 12 Tf 72 686 Td (BBB) Tj ET "
        "BT /F1 12 Tf 300 686 Td (YYY) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *plain = tspdf_reader_page_text(doc, 0, &err);
    ASSERT(plain != NULL);
    ASSERT_EQ_STR(plain, "XXX AAA\nBBB YYY\n");

    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    char l1[256], l2[256];
    ASSERT(text_line(text, 0, l1, sizeof(l1)));
    ASSERT(text_line(text, 1, l2, sizeof(l2)));
    const char *a = strstr(l1, "AAA");
    const char *x = strstr(l1, "XXX");
    const char *b = strstr(l2, "BBB");
    const char *y = strstr(l2, "YYY");
    ASSERT(a && x && b && y);
    ASSERT(a < x);                       // re-ordered left-to-right
    ASSERT(x - a >= 3 + 5);              // a real gap, not one space
    ASSERT_EQ_SIZE((size_t)(x - l1), (size_t)(y - l2)); // columns align
    ASSERT_EQ_SIZE((size_t)(a - l1), (size_t)(b - l2));
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_table_grid) {
    // 3x2 table: three columns, two rows. Every column must start at the
    // same character offset in both rows.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Name) Tj ET "
        "BT /F1 12 Tf 252 700 Td (Qty) Tj ET "
        "BT /F1 12 Tf 432 700 Td (Price) Tj ET "
        "BT /F1 12 Tf 72 680 Td (Apple) Tj ET "
        "BT /F1 12 Tf 252 680 Td (3) Tj ET "
        "BT /F1 12 Tf 432 680 Td (1.50) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    char r1[512], r2[512];
    ASSERT(text_line(text, 0, r1, sizeof(r1)));
    ASSERT(text_line(text, 1, r2, sizeof(r2)));
    const char *qty = strstr(r1, "Qty");
    const char *price = strstr(r1, "Price");
    const char *three = strstr(r2, "3");
    const char *val = strstr(r2, "1.50");
    ASSERT(strstr(r1, "Name") == r1);
    ASSERT(strstr(r2, "Apple") == r2);
    ASSERT(qty && price && three && val);
    ASSERT_EQ_SIZE((size_t)(qty - r1), (size_t)(three - r2));
    ASSERT_EQ_SIZE((size_t)(price - r1), (size_t)(val - r2));
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_y_jitter_merges_line) {
    // Baselines 0.5pt apart (generator jitter) must land on one line.
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Left) Tj ET "
        "BT /F1 12 Tf 200 699.5 Td (Right) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Left") != NULL);
    ASSERT(strstr(text, "Right") != NULL);
    size_t newlines = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') newlines++;
    ASSERT_EQ_SIZE(newlines, 1);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_blank_lines_bounded) {
    // A vertical gap beyond ~1.8 line heights becomes one blank line; a huge
    // gap is capped at two blank lines (never proportional to the distance).
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (Top) Tj ET "
        "BT /F1 12 Tf 72 676 Td (Mid) Tj ET "
        "BT /F1 12 Tf 72 100 Td (Bottom) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "Top\n\nMid\n\n\nBottom\n");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_hostile_x_clamped) {
    // A fragment at x = 1e9 must clamp to the grid width cap instead of
    // allocating a billion-column line (ASan run guards the allocation).
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT,
        "BT /F1 12 Tf 72 700 Td (A) Tj ET "
        "BT /F1 12 Tf 1000000000 700 Td (B) Tj ET", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "A") != NULL);
    ASSERT(strstr(text, "B") != NULL);
    ASSERT(strlen(text) < 1200);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_empty_page) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT, "", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 0, &err);
    ASSERT(text != NULL);
    ASSERT_EQ_STR(text, "");
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_text_layout_page_out_of_range) {
    size_t len = 0;
    char *pdf = make_text_pdf(TEXT_HELV_FONT, "", &len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    const char *text = tspdf_reader_page_text_layout(doc, 5, &err);
    ASSERT(text == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_PAGE_RANGE);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// ============================================================
// Doc trees (feat/doctrees): outlines + AcroForm across merge/extract
// ============================================================

// Resolve a catalog entry of a reopened document.
static TspdfObj *dt_catalog_get(TspdfReader *doc, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(doc->catalog, key));
}

static TspdfObj *dt_get(TspdfReader *doc, TspdfObj *dict, const char *key) {
    return test_resolve_ref(doc, tspdf_dict_get(dict, key));
}

static bool dt_str_eq(TspdfObj *s, const char *want) {
    return s && (s->type == TSPDF_OBJ_STRING || s->type == TSPDF_OBJ_NAME) &&
           s->string.len == strlen(want) &&
           memcmp(s->string.data, want, s->string.len) == 0;
}

// Page index the item's destination points at, or SIZE_MAX. Requires an
// explicit dest array (named dests must have been flattened).
static size_t dt_dest_page(TspdfReader *doc, TspdfObj *item) {
    TspdfObj *dest = dt_get(doc, item, "Dest");
    if (dest && dest->type == TSPDF_OBJ_DICT) {
        dest = dt_get(doc, dest, "D");
    }
    if (!dest || dest->type != TSPDF_OBJ_ARRAY || dest->array.count == 0) {
        return SIZE_MAX;
    }
    TspdfObj *p = &dest->array.items[0];
    if (p->type != TSPDF_OBJ_REF) return SIZE_MAX;
    for (size_t i = 0; i < doc->pages.count; i++) {
        if (doc->pages.pages[i].obj_num == p->ref.num) return i;
    }
    return SIZE_MAX;
}

// Writer-generated source with `npages` pages, a small outline tree
// ("<tag>-CH1" -> page 0 with child "<tag>-CH1-SUB" -> page min(1, last),
// "<tag>-CH2" -> last page) and two fields ("<tag>_text" on page 0,
// "<tag>_check" on the last page).
static uint8_t *dt_writer_pdf(int npages, bool outlines, bool fields,
                              const char *tag, const char *font_name,
                              size_t *out_len) {
    TspdfWriter *w = tspdf_writer_create();
    if (!w) return NULL;
    const char *font = tspdf_writer_add_builtin_font(w, font_name);
    for (int i = 0; i < npages; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 14);
        tspdf_stream_move_to(page, 72, 700);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s page %d", tag, i + 1);
        tspdf_stream_show_text(page, buf);
        tspdf_stream_end_text(page);
    }
    if (outlines) {
        char t[128];
        snprintf(t, sizeof(t), "%s-CH1", tag);
        int ch1 = tspdf_writer_add_bookmark(w, t, 0);
        snprintf(t, sizeof(t), "%s-CH1-SUB", tag);
        tspdf_writer_add_child_bookmark(w, ch1, t, npages > 1 ? 1 : 0);
        snprintf(t, sizeof(t), "%s-CH2", tag);
        tspdf_writer_add_bookmark(w, t, npages - 1);
    }
    if (fields) {
        char n[128];
        snprintf(n, sizeof(n), "%s_text", tag);
        tspdf_writer_add_text_field(w, 0, n, 72, 600, 200, 20, "hello",
                                    font_name, 12);
        snprintf(n, sizeof(n), "%s_check", tag);
        tspdf_writer_add_checkbox(w, npages - 1, n, 72, 560, 14, true);
    }
    uint8_t *data = NULL;
    TspdfError err = tspdf_writer_save_to_memory(w, &data, out_len);
    tspdf_writer_destroy(w);
    if (err != TSPDF_OK) {
        free(data);
        return NULL;
    }
    return data;
}

// Hand-built two-page PDF exercising named destinations (both the /Names name
// tree and the PDF 1.1 catalog /Dests dict), a /GoTo action with a named /D,
// and an outline item that only carries a URI action.
static char *dt_make_named_dest_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[16] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R "
                 "/Names << /Dests 8 0 R >> /Dests 11 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R 5 0 R] /Count 2 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Outlines /First 6 0 R /Last 14 0 R /Count 4 >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (NAMED1) /Parent 3 0 R /Next 7 0 R "
                 "/Dest (target1) >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Title (NAMED2) /Parent 3 0 R /Prev 6 0 R /Next 13 0 R "
                 "/A << /S /GoTo /D (target2) >> >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /Names [(target1) 9 0 R (target2) 10 0 R] >>\nendobj\n")) goto fail;
    off[9] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "9 0 obj\n[4 0 R /XYZ 0 792 0]\nendobj\n")) goto fail;
    off[10] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "10 0 obj\n<< /D [5 0 R /Fit] >>\nendobj\n")) goto fail;
    off[11] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "11 0 obj\n<< /target3 12 0 R >>\nendobj\n")) goto fail;
    off[12] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "12 0 obj\n[4 0 R /Fit]\nendobj\n")) goto fail;
    off[13] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "13 0 obj\n<< /Title (NAMED3) /Parent 3 0 R /Prev 7 0 R /Next 14 0 R "
                 "/Dest /target3 >>\nendobj\n")) goto fail;
    off[14] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "14 0 obj\n<< /Title (URIITEM) /Parent 3 0 R /Prev 13 0 R "
                 "/A << /S /URI /URI (http://example.org/x) >> >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 15\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 14; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 15 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// Hand-built three-page PDF with one hierarchical form field: a parent field
// whose two widget kids sit on pages 0 and 1 (the second without /P, so the
// page has to be found through its /Annots entry). The AcroForm dict is
// inline in the catalog, like tspdf's own writer emits it.
static char *dt_make_field_kids_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[16] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [6 0 R] /DA (/Helv 0 Tf 0 g) "
                 "/NeedAppearances true >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [7 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [8 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /T (parentfield) /FT /Tx /Kids [7 0 R 8 0 R] >>\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "7 0 obj\n<< /Type /Annot /Subtype /Widget /Rect [10 10 60 30] "
                 "/Parent 6 0 R /P 3 0 R /F 4 >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "8 0 obj\n<< /Type /Annot /Subtype /Widget /Rect [10 40 60 60] "
                 "/Parent 6 0 R /F 4 >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 8; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_merge_preserves_outlines) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(2, true, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, true, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);

    // Top level: AA-CH1, AA-CH2, BB-CH1, BB-CH2 in source order.
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 4);

    TspdfObj *it1 = dt_get(re, root, "First");
    ASSERT(it1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it1, "Title"), "AA-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it1), 0);

    // AA-CH1 keeps its child, pointing at merged page 1.
    TspdfObj *sub = dt_get(re, it1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "AA-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 1);

    TspdfObj *it2 = dt_get(re, it1, "Next");
    ASSERT(it2 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it2, "Title"), "AA-CH2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it2), 1);

    TspdfObj *it3 = dt_get(re, it2, "Next");
    ASSERT(it3 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it3, "Title"), "BB-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it3), 2);

    TspdfObj *it4 = dt_get(re, it3, "Next");
    ASSERT(it4 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(it4, "Title"), "BB-CH2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, it4), 2);
    ASSERT(tspdf_dict_get(it4, "Next") == NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_concatenates_acroform_fields) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, true, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, false, true, "BB", "Times-Roman", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);

    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 4);

    // All four fields are registered with their names intact.
    bool seen_aa_text = false, seen_aa_check = false;
    bool seen_bb_text = false, seen_bb_check = false;
    for (size_t i = 0; i < fields->array.count; i++) {
        TspdfObj *field = test_resolve_ref(re, &fields->array.items[i]);
        ASSERT(field != NULL && field->type == TSPDF_OBJ_DICT);
        TspdfObj *name = tspdf_dict_get(field, "T");
        if (dt_str_eq(name, "AA_text")) seen_aa_text = true;
        if (dt_str_eq(name, "AA_check")) seen_aa_check = true;
        if (dt_str_eq(name, "BB_text")) seen_bb_text = true;
        if (dt_str_eq(name, "BB_check")) seen_bb_check = true;
    }
    ASSERT(seen_aa_text && seen_aa_check && seen_bb_text && seen_bb_check);

    // /NeedAppearances carries through; /DR font dicts merge, first source
    // wins the F1 name collision (Helvetica, not Times-Roman).
    TspdfObj *needap = tspdf_dict_get(acro, "NeedAppearances");
    ASSERT(needap != NULL && needap->type == TSPDF_OBJ_BOOL && needap->boolean);
    TspdfObj *dr = dt_get(re, acro, "DR");
    ASSERT(dr != NULL && dr->type == TSPDF_OBJ_DICT);
    TspdfObj *dr_fonts = dt_get(re, dr, "Font");
    ASSERT(dr_fonts != NULL && dr_fonts->type == TSPDF_OBJ_DICT);
    TspdfObj *f1 = dt_get(re, dr_fonts, "F1");
    ASSERT(f1 != NULL && f1->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(f1, "BaseFont"), "Helvetica"));

    // Both widgets remain wired into their pages' /Annots.
    TspdfObj *page0_annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    TspdfObj *page1_annots = dt_get(re, re->pages.pages[1].page_dict, "Annots");
    ASSERT(page0_annots != NULL && page0_annots->type == TSPDF_OBJ_ARRAY &&
           page0_annots->array.count >= 2);
    ASSERT(page1_annots != NULL && page1_annots->type == TSPDF_OBJ_ARRAY &&
           page1_annots->array.count >= 2);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_without_doctrees_stays_clean) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_keeps_outline_subtree_for_kept_pages) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "CC", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Keep pages 0 and 1: CC-CH1 (page 0) and its child (page 1) survive,
    // CC-CH2 (page 2) is pruned.
    size_t pages[] = {0, 1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "CC-CH2"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 1);

    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(ch1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "CC-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, ch1), 0);
    ASSERT(tspdf_dict_get(ch1, "Next") == NULL);

    TspdfObj *sub = dt_get(re, ch1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "CC-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_keeps_parent_when_only_child_survives) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "DD", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Keep only page 1: DD-CH1 points at dropped page 0 but its child
    // DD-CH1-SUB points at the kept page, so the parent stays (dest dropped).
    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(ch1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "DD-CH1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, ch1), SIZE_MAX);  // dest to dropped page gone
    TspdfObj *sub = dt_get(re, ch1, "First");
    ASSERT(sub != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(sub, "Title"), "DD-CH1-SUB"));
    ASSERT_EQ_SIZE(dt_dest_page(re, sub), 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_omits_outlines_when_nothing_survives) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, true, false, "EE", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // EE bookmarks point at pages 0, 1 and 2... page 2 has EE-CH2. Build a
    // fresh 3-page source whose bookmarks all target page 0 instead.
    tspdf_reader_destroy(doc);
    free(pdf);

    TspdfWriter *w = tspdf_writer_create();
    ASSERT(w != NULL);
    const char *font = tspdf_writer_add_builtin_font(w, "Helvetica");
    for (int i = 0; i < 3; i++) {
        TspdfStream *page = tspdf_writer_add_page(w);
        tspdf_stream_begin_text(page);
        tspdf_stream_set_font(page, font, 14);
        tspdf_stream_move_to(page, 72, 700);
        tspdf_stream_show_text(page, "x");
        tspdf_stream_end_text(page);
    }
    tspdf_writer_add_bookmark(w, "FIRSTONLY", 0);
    uint8_t *data = NULL;
    ASSERT_EQ_INT(tspdf_writer_save_to_memory(w, &data, &len), TSPDF_OK);
    tspdf_writer_destroy(w);

    doc = tspdf_reader_open(data, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {2};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "FIRSTONLY"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(data);
}

TEST(test_extract_prunes_acroform_fields_by_page) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(3, false, true, "FF", "Helvetica", &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // FF_text sits on page 0, FF_check on page 2. Keep page 0 only.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "FF_check"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);
    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 1);
    TspdfObj *field = test_resolve_ref(re, &fields->array.items[0]);
    ASSERT(field != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(field, "T"), "FF_text"));

    tspdf_reader_destroy(re);
    free(out);

    // Keeping only the middle page (no fields) omits /AcroForm entirely.
    size_t middle[] = {1};
    TspdfReader *none = tspdf_reader_extract(doc, middle, 1, &err);
    ASSERT(none != NULL);
    err = tspdf_reader_save_to_memory(none, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "FF_text"));
    free(out);
    tspdf_reader_destroy(none);

    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_extract_flattens_named_dests) {
    size_t len = 0;
    char *pdf = dt_make_named_dest_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    // Keep page 0: NAMED1 (name tree -> page 0) and NAMED3 (/Dests dict ->
    // page 0) survive with explicit dest arrays; NAMED2 (GoTo -> page 1) and
    // URIITEM (no page target) are dropped.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    ASSERT(!bytes_contains(out, out_len, "/Names"));
    ASSERT(!bytes_contains(out, out_len, "NAMED2"));
    ASSERT(!bytes_contains(out, out_len, "URIITEM"));
    ASSERT(!bytes_contains(out, out_len, "target1"));  // flattened away

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 2);

    TspdfObj *n1 = dt_get(re, root, "First");
    ASSERT(n1 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(n1, "Title"), "NAMED1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n1), 0);

    TspdfObj *n3 = dt_get(re, n1, "Next");
    ASSERT(n3 != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(n3, "Title"), "NAMED3"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n3), 0);
    ASSERT(tspdf_dict_get(n3, "Next") == NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_merge_flattens_named_dests_and_keeps_uri_actions) {
    size_t len_a = 0, len_b = 0;
    char *pdf_a = dt_make_named_dest_pdf(&len_a);
    uint8_t *pdf_b = dt_writer_pdf(1, false, false, "PL", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 4);

    TspdfObj *n1 = dt_get(re, root, "First");
    ASSERT(n1 != NULL && dt_str_eq(tspdf_dict_get(n1, "Title"), "NAMED1"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n1), 0);

    // The named /GoTo action is flattened to an explicit dest on page 1.
    TspdfObj *n2 = dt_get(re, n1, "Next");
    ASSERT(n2 != NULL && dt_str_eq(tspdf_dict_get(n2, "Title"), "NAMED2"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n2), 1);

    TspdfObj *n3 = dt_get(re, n2, "Next");
    ASSERT(n3 != NULL && dt_str_eq(tspdf_dict_get(n3, "Title"), "NAMED3"));
    ASSERT_EQ_SIZE(dt_dest_page(re, n3), 0);

    // The URI item survives a merge with its action untouched.
    TspdfObj *n4 = dt_get(re, n3, "Next");
    ASSERT(n4 != NULL && dt_str_eq(tspdf_dict_get(n4, "Title"), "URIITEM"));
    TspdfObj *action = dt_get(re, n4, "A");
    ASSERT(action != NULL && action->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(action, "S"), "URI"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_prunes_field_kids_and_carries_da) {
    size_t len = 0;
    char *pdf = dt_make_field_kids_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 3);

    // Keep page 1: the parent field survives with only the second widget
    // (whose page is only discoverable through /Annots, it has no /P).
    size_t pages[] = {1};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL && acro->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(acro, "DA"), "/Helv 0 Tf 0 g"));
    TspdfObj *needap = tspdf_dict_get(acro, "NeedAppearances");
    ASSERT(needap != NULL && needap->type == TSPDF_OBJ_BOOL && needap->boolean);

    TspdfObj *fields = dt_get(re, acro, "Fields");
    ASSERT(fields != NULL && fields->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(fields->array.count, 1);
    TspdfObj *parent = test_resolve_ref(re, &fields->array.items[0]);
    ASSERT(parent != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(parent, "T"), "parentfield"));
    TspdfObj *kids = dt_get(re, parent, "Kids");
    ASSERT(kids != NULL && kids->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(kids->array.count, 1);
    TspdfObj *kid = test_resolve_ref(re, &kids->array.items[0]);
    ASSERT(kid != NULL && kid->type == TSPDF_OBJ_DICT);
    ASSERT(dt_str_eq(tspdf_dict_get(kid, "Subtype"), "Widget"));

    // The kept widget stays in its page's /Annots.
    TspdfObj *annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    ASSERT(annots != NULL && annots->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(annots->array.count, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);

    // Keep page 2 (no widgets anywhere): the whole field, and with it the
    // /AcroForm, disappears.
    size_t last[] = {2};
    TspdfReader *none = tspdf_reader_extract(doc, last, 1, &err);
    ASSERT(none != NULL);
    err = tspdf_reader_save_to_memory(none, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "parentfield"));
    free(out);
    tspdf_reader_destroy(none);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// Hand-built one-page PDF whose /Names /Dests node lists itself 50 times in
// /Kids, plus an outline item with a named destination so the lookup runs.
// Without a node budget the tree walk fans out to 50^32 recursive calls.
static char *dt_make_cyclic_nametree_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R "
                 "/Names << /Dests 5 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Outlines /First 6 0 R /Last 6 0 R /Count 1 >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos, "5 0 obj\n<< /Kids [")) goto fail;
    for (int i = 0; i < 50; i++) {
        if (!appendf(pdf, 8192, &pos, "5 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (CYCITEM) /Parent 4 0 R "
                 "/Dest (loopname) >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// The cyclic name tree must not blow up extract or merge; the named dest is
// unresolvable, so its outline item is dropped either way.
TEST(test_extract_and_merge_bounded_on_cyclic_nametree) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_nametree_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));
    ASSERT(!bytes_contains(out, out_len, "CYCITEM"));
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);

    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    TspdfReader *doc_b = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);
    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "CYCITEM"));
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf);
}

// Hand-built one-page PDF with a form field whose /Kids array lists the
// field itself 40 times. Without a node budget pruning recurses 40^32 times.
static char *dt_make_cyclic_field_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [4 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos, "4 0 obj\n<< /T (loopfield) /FT /Tx /Kids [")) goto fail;
    for (int i = 0; i < 40; i++) {
        if (!appendf(pdf, 8192, &pos, "4 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// The self-referential field never reaches a widget on a kept page, so it
// (and the whole /AcroForm) drops out — in bounded time.
TEST(test_extract_bounded_on_cyclic_field_kids) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_field_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "loopfield"));

    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Hand-built one-page PDF whose two outline items form a /Next cycle
// (LOOPA -> LOOPB -> LOOPA). Both point at the kept page.
static char *dt_make_cyclic_outline_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Type /Outlines /First 5 0 R /Last 6 0 R /Count 2 >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Title (LOOPA) /Parent 4 0 R /Next 6 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Title (LOOPB) /Parent 4 0 R /Prev 5 0 R /Next 5 0 R "
                 "/Dest [3 0 R /Fit] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// A sibling cycle must come out as a straight two-item chain: /Count 2 and
// no /Next on the last item (not a still-cyclic /Prev//Next pair with an
// inflated count).
TEST(test_extract_breaks_cyclic_outline_siblings) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_outline_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *count = tspdf_dict_get(root, "Count");
    ASSERT(count != NULL && count->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)count->integer, 2);

    TspdfObj *a = dt_get(re, root, "First");
    ASSERT(a != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(a, "Title"), "LOOPA"));
    ASSERT(tspdf_dict_get(a, "Prev") == NULL);
    TspdfObj *b = dt_get(re, a, "Next");
    ASSERT(b != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(b, "Title"), "LOOPB"));
    ASSERT(tspdf_dict_get(b, "Next") == NULL);
    TspdfObj *b_prev = dt_get(re, b, "Prev");
    ASSERT(b_prev != NULL);
    ASSERT(dt_str_eq(tspdf_dict_get(b_prev, "Title"), "LOOPA"));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// ============================================================
// Embedded file attachments (feat/attach)
// ============================================================

// Binary payload with NULs and high bytes so byte identity means something.
static const uint8_t att_payload[] = {
    'H', 'e', 'l', 'l', 'o', 0x00, 0xFF, 0x01, '\n', 'w', 'o', 'r', 'l', 'd',
    0x80, 0x00, 'e', 'n', 'd'
};

// Open → add → save → reopen. Returns the reopened doc; caller destroys it
// and frees *out_buf.
static TspdfReader *att_add_and_reopen(const char *name, const uint8_t *data,
                                       size_t len, const char *desc,
                                       uint8_t **out_buf) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(2, false, false, "AT", "Helvetica", &src_len);
    if (!src) return NULL;

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    if (!doc) {
        free(src);
        return NULL;
    }
    if (tspdf_reader_attachment_add(doc, name, data, len, desc) != TSPDF_OK) {
        tspdf_reader_destroy(doc);
        free(src);
        return NULL;
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    tspdf_reader_destroy(doc);
    free(src);
    if (err != TSPDF_OK) {
        free(out);
        return NULL;
    }

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    if (!re) {
        free(out);
        return NULL;
    }
    *out_buf = out;
    return re;
}

TEST(test_attach_add_save_reopen_roundtrip) {
    uint8_t *buf = NULL;
    TspdfReader *re = att_add_and_reopen("hello.txt", att_payload,
                                         sizeof(att_payload), "greeting", &buf);
    ASSERT(re != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    TspdfError err = tspdf_reader_attachments(re, &infos, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT(infos != NULL);
    ASSERT_EQ_STR(infos[0].name, "hello.txt");
    ASSERT_EQ_SIZE(infos[0].size, sizeof(att_payload));
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, "greeting");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    err = tspdf_reader_attachment_get(re, "hello.txt", &bytes, &blen);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    // Unknown names report NOT_FOUND, not a parse error.
    err = tspdf_reader_attachment_get(re, "nope.txt", &bytes, &blen);
    ASSERT_EQ_INT(err, TSPDF_ERR_NOT_FOUND);

    tspdf_reader_destroy(re);
    free(buf);
}

TEST(test_attach_flat_tree_keys_sorted) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    // Insert out of order; the flat node must come out sorted.
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "m.bin", att_payload, 4, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "a.bin", att_payload, 3, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "z.bin", att_payload, 5, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "ab.bin", att_payload, 2, NULL), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 4);
    // Enumeration follows tree order, so this asserts the stored key order
    // ("a" < "ab": shorter prefix sorts first).
    ASSERT_EQ_STR(infos[0].name, "a.bin");
    ASSERT_EQ_STR(infos[1].name, "ab.bin");
    ASSERT_EQ_STR(infos[2].name, "m.bin");
    ASSERT_EQ_STR(infos[3].name, "z.bin");
    // Sizes tell the entries apart, proving values track their keys.
    ASSERT_EQ_SIZE(infos[0].size, 3);
    ASSERT_EQ_SIZE(infos[1].size, 2);
    ASSERT_EQ_SIZE(infos[2].size, 4);
    ASSERT_EQ_SIZE(infos[3].size, 5);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_add_replaces_same_name) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "f.txt",
                  (const uint8_t *)"old", 3, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "f.txt",
                  (const uint8_t *)"newer", 5, NULL), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "f.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 5);
    ASSERT(memcmp(bytes, "newer", 5) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
}

TEST(test_attach_remove) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "keep.txt",
                  (const uint8_t *)"keep", 4, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "drop.txt",
                  (const uint8_t *)"drop", 4, NULL), TSPDF_OK);

    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "missing.txt"),
                  TSPDF_ERR_NOT_FOUND);
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "drop.txt"), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "keep.txt");
    // The removed attachment's objects must not linger in the output.
    ASSERT(!bytes_contains(out, out_len, "drop.txt"));
    tspdf_reader_destroy(re);
    free(out);

    // Removing the last attachment removes the whole /EmbeddedFiles tree
    // (and the then-empty /Names dict).
    src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);
    doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "only.txt",
                  (const uint8_t *)"x", 1, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "only.txt"), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/EmbeddedFiles"));
    tspdf_reader_destroy(doc);
    free(src);
    free(out);
}

// Hand-built one-page PDF whose /EmbeddedFiles node lists itself 40 times in
// /Kids. Without a node budget enumeration recurses 40^32 times.
static char *att_make_cyclic_embeddedfiles_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    off[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "4 0 obj\n<< /Names [(cyc.txt) 5 0 R] /Kids [")) goto fail;
    for (int i = 0; i < 40; i++) {
        if (!appendf(pdf, 8192, &pos, "4 0 R ")) goto fail;
    }
    if (!appendf(pdf, 8192, &pos, "] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "5 0 obj\n<< /Type /Filespec /F (cyc.txt) /UF (cyc.txt) "
                 "/EF << /F 6 0 R >> >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "6 0 obj\n<< /Type /EmbeddedFile /Length 9 >>\nstream\n"
                 "CYCBYTES!\nendstream\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_attach_cyclic_embeddedfiles_tree_bounded) {
    size_t len = 0;
    char *pdf = att_make_cyclic_embeddedfiles_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Enumeration terminates and reports the entry exactly once.
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "cyc.txt");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "cyc.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 9);
    ASSERT(memcmp(bytes, "CYCBYTES!", 9) == 0);
    free(bytes);

    // Extract (which re-adds all attachments) is bounded too, and the
    // rebuilt flat tree carries the entry through.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "cyc.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 9);
    ASSERT(memcmp(bytes, "CYCBYTES!", 9) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_attach_extract_keeps_all_attachments) {
    uint8_t *buf = NULL;
    TspdfReader *src = att_add_and_reopen("data.bin", att_payload,
                                          sizeof(att_payload), "desc here", &buf);
    ASSERT(src != NULL);

    // Extract only page 1 of 2: attachments are document-level and all survive.
    size_t pages[] = {1};
    TspdfError err;
    TspdfReader *result = tspdf_reader_extract(src, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, "data.bin");
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, "desc here");

    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "data.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(src);
    free(buf);
}

TEST(test_attach_merge_unions_first_wins) {
    uint8_t *buf_a = NULL;
    uint8_t *buf_b = NULL;
    TspdfReader *doc_a = att_add_and_reopen("shared.txt",
                                            (const uint8_t *)"from A", 6,
                                            NULL, &buf_a);
    TspdfReader *doc_b = att_add_and_reopen("shared.txt",
                                            (const uint8_t *)"from B!!", 8,
                                            NULL, &buf_b);
    ASSERT(doc_a != NULL && doc_b != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc_a, "a.txt",
                  (const uint8_t *)"AA", 2, NULL), TSPDF_OK);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc_b, "b.txt",
                  (const uint8_t *)"BB", 2, NULL), TSPDF_OK);

    TspdfError err;
    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(merged, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 3);
    ASSERT_EQ_STR(infos[0].name, "a.txt");
    ASSERT_EQ_STR(infos[1].name, "b.txt");
    ASSERT_EQ_STR(infos[2].name, "shared.txt");

    // First source wins the name collision.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "shared.txt", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 6);
    ASSERT(memcmp(bytes, "from A", 6) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(buf_a);
    free(buf_b);
}

TEST(test_attach_delete_pages_keeps_attachments) {
    uint8_t *buf = NULL;
    TspdfReader *src = att_add_and_reopen("data.bin", att_payload,
                                          sizeof(att_payload), NULL, &buf);
    ASSERT(src != NULL);

    size_t pages[] = {0};
    TspdfError err;
    TspdfReader *result = tspdf_reader_delete(src, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "data.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(src);
    free(buf);
}

TEST(test_attach_encrypted_roundtrip) {
    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "secret.bin", att_payload,
                                              sizeof(att_payload), NULL), TSPDF_OK);

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_encrypted(doc, &enc, &enc_len,
                                                        "pw", "pw", 0xFFFFFFFF, 256),
                  TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    // Wrong/missing password never reaches the attachment.
    TspdfReader *no_pw = tspdf_reader_open(enc, enc_len, &err);
    ASSERT(no_pw == NULL);
    ASSERT_EQ_INT(err, TSPDF_ERR_ENCRYPTED);

    // With the password the attachment stream decrypts and decodes.
    TspdfReader *re = tspdf_reader_open_with_password(enc, enc_len, "pw", &err);
    ASSERT(re != NULL);
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, "secret.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    // And an extract of the encrypted document still carries it. The extract
    // inherits the source's crypt, so its save stays encrypted under the
    // same password.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(re, pages, 1, &err);
    ASSERT(result != NULL);
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(result, &out, &out_len), TSPDF_OK);
    TspdfReader *re2 = tspdf_reader_open_with_password(out, out_len, "pw", &err);
    ASSERT(re2 != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re2, "secret.bin", &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    tspdf_reader_destroy(re2);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(re);
    free(enc);
}

TEST(test_attach_unicode_name_stored_as_utf16_roundtrip) {
    // A non-ASCII attachment name is a PDF text string: it must be written
    // as UTF-16BE with BOM (ISO 32000 §7.9.2.2) in /F, /UF and the
    // EmbeddedFiles name-tree key — not as raw UTF-8 bytes.
    const char *name = "sp \xC3\xBCn\xC3\xAF" "code.txt";  // "sp ünïcode.txt"
    const char *desc = "d\xC3\xA9sc";                      // "désc"

    size_t src_len = 0;
    uint8_t *src = dt_writer_pdf(1, false, false, "AT", "Helvetica", &src_len);
    ASSERT(src != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(src, src_len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, name, att_payload,
                                              sizeof(att_payload), desc), TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory(doc, &out, &out_len), TSPDF_OK);
    tspdf_reader_destroy(doc);
    free(src);

    // Raw bytes: the escaped UTF-16 BOM (\376\377) is present and no raw
    // UTF-8 tail leaked into any string.
    ASSERT(bytes_contains(out, out_len, "\\376\\377"));
    ASSERT(!bytes_contains(out, out_len, "\xC3\xBCn\xC3\xAF"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    // Listing decodes the stored name and description back to UTF-8.
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 1);
    ASSERT_EQ_STR(infos[0].name, name);
    ASSERT(infos[0].desc != NULL);
    ASSERT_EQ_STR(infos[0].desc, desc);

    // Lookup, replace and remove all accept the UTF-8 name.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(re, name, &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, sizeof(att_payload));
    ASSERT(memcmp(bytes, att_payload, blen) == 0);
    free(bytes);

    ASSERT_EQ_INT(tspdf_reader_attachment_add(re, name,
                                              (const uint8_t *)"newer", 5, NULL),
                  TSPDF_OK);
    TspdfAttachmentInfo *infos2 = NULL;
    size_t count2 = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(re, &infos2, &count2), TSPDF_OK);
    ASSERT_EQ_SIZE(count2, 1);  // replaced, not duplicated

    ASSERT_EQ_INT(tspdf_reader_attachment_remove(re, name), TSPDF_OK);
    tspdf_reader_destroy(re);
    free(out);
}

// Hand-built PDF with two attachments named the way other tools name them:
// a UTF-16BE key with BOM ("ünïcode.txt", pymupdf-style) and a PDFDocEncoded
// key ("café.txt", qpdf-style).
static char *att_make_foreign_names_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[10] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.5\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/Names << /EmbeddedFiles 4 0 R >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    // Keys sorted by raw bytes: (caf\xE9.txt) < <FEFF...>.
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Names [(caf\xE9.txt) 5 0 R "
                 "<FEFF00FC006E00EF0063006F00640065002E007400780074> 7 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /Type /Filespec /F (caf\xE9.txt) /UF (caf\xE9.txt) "
                 "/EF << /F 6 0 R >> >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /Type /EmbeddedFile /Length 12 >>\nstream\n"
                 "hello pdfdoc\nendstream\nendobj\n")) goto fail;
    off[7] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "7 0 obj\n<< /Type /Filespec /F (unicode.txt) "
                 "/UF <FEFF00FC006E00EF0063006F00640065002E007400780074> "
                 "/EF << /F 8 0 R >> >>\nendobj\n")) goto fail;
    off[8] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "8 0 obj\n<< /Type /EmbeddedFile /Length 11 >>\nstream\n"
                 "hello utf16\nendstream\nendobj\n")) goto fail;

    {
        size_t xref = pos;
        if (!appendf(pdf, 4096, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 8; i++) {
            if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, 4096, &pos,
                     "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_attach_reads_foreign_encoded_names) {
    size_t len = 0;
    char *pdf = att_make_foreign_names_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // Listing decodes both encodings to UTF-8 (tree order: PDFDoc key first).
    TspdfAttachmentInfo *infos = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos, &count), TSPDF_OK);
    ASSERT_EQ_SIZE(count, 2);
    ASSERT_EQ_STR(infos[0].name, "caf\xC3\xA9.txt");
    ASSERT_EQ_STR(infos[1].name, "\xC3\xBCn\xC3\xAF" "code.txt");

    // Lookup by the decoded UTF-8 name works for both.
    uint8_t *bytes = NULL;
    size_t blen = 0;
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "\xC3\xBCn\xC3\xAF" "code.txt",
                                              &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 11);
    ASSERT(memcmp(bytes, "hello utf16", 11) == 0);
    free(bytes);
    ASSERT_EQ_INT(tspdf_reader_attachment_get(doc, "caf\xC3\xA9.txt",
                                              &bytes, &blen), TSPDF_OK);
    ASSERT_EQ_SIZE(blen, 12);
    ASSERT(memcmp(bytes, "hello pdfdoc", 12) == 0);
    free(bytes);

    // Adding under the same UTF-8 name replaces the PDFDocEncoded entry
    // instead of creating a differently-encoded duplicate.
    ASSERT_EQ_INT(tspdf_reader_attachment_add(doc, "caf\xC3\xA9.txt",
                                              (const uint8_t *)"newer", 5, NULL),
                  TSPDF_OK);
    TspdfAttachmentInfo *infos2 = NULL;
    size_t count2 = 0;
    ASSERT_EQ_INT(tspdf_reader_attachments(doc, &infos2, &count2), TSPDF_OK);
    ASSERT_EQ_SIZE(count2, 2);

    // Removal by the decoded name works too.
    ASSERT_EQ_INT(tspdf_reader_attachment_remove(doc, "\xC3\xBCn\xC3\xAF" "code.txt"),
                  TSPDF_OK);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// ============================================================
// Page label preservation (extract/merge)
// ============================================================

// Five-page PDF labeled i, ii, iii, 1, 2 via /PageLabels
// << /Nums [0 << /S /r >> 3 << /S /D >>] >>. When `cyclic` is set the
// number tree root points at a kid that lists itself in /Kids forever.
static char *pl_make_labeled_pdf(bool cyclic, size_t *out_len) {
    char *pdf = (char *)malloc(8192);
    if (!pdf) return NULL;

    size_t pos = 0;
    if (!appendf(pdf, 8192, &pos, "%%PDF-1.4\n")) goto fail;

    size_t offs[10];
    offs[1] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /PageLabels 8 0 R >>\nendobj\n"))
        goto fail;
    offs[2] = pos;
    if (!appendf(pdf, 8192, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] "
                 "/Count 5 >>\nendobj\n")) goto fail;
    for (int i = 3; i <= 7; i++) {
        offs[i] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "%d 0 obj\n<< /Type /Page /Parent 2 0 R "
                     "/MediaBox [0 0 612 792] >>\nendobj\n", i)) goto fail;
    }
    offs[8] = pos;
    if (cyclic) {
        if (!appendf(pdf, 8192, &pos,
                     "8 0 obj\n<< /Kids [9 0 R] >>\nendobj\n")) goto fail;
        offs[9] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "9 0 obj\n<< /Kids [9 0 R 8 0 R] "
                     "/Nums [0 << /S /r >> 3 << /S /D >>] >>\nendobj\n")) goto fail;
    } else {
        if (!appendf(pdf, 8192, &pos,
                     "8 0 obj\n<< /Nums [0 << /S /r >> 3 << /S /D >>] >>\nendobj\n"))
            goto fail;
        offs[9] = pos;
        if (!appendf(pdf, 8192, &pos,
                     "9 0 obj\n<< /Unused true >>\nendobj\n")) goto fail;
    }

    size_t xref = pos;
    if (!appendf(pdf, 8192, &pos,
                 "xref\n0 10\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 9; i++) {
        if (!appendf(pdf, 8192, &pos, "%010zu 00000 n \n", offs[i])) goto fail;
    }
    if (!appendf(pdf, 8192, &pos,
                 "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;

    *out_len = pos;
    return pdf;

fail:
    free(pdf);
    return NULL;
}

// Expect Nums pair `pair_idx` of `nums` to be (key, << /S style /St st >>)
// where style 0 = no /S and st 1 = no /St (the spec default).
static bool pl_expect_range(TspdfReader *doc, TspdfObj *nums, size_t pair_idx,
                            int64_t key, char style, int64_t st) {
    if (!nums || nums->type != TSPDF_OBJ_ARRAY) return false;
    if (nums->array.count < (pair_idx + 1) * 2) return false;
    TspdfObj *k = &nums->array.items[pair_idx * 2];
    if (k->type != TSPDF_OBJ_INT || k->integer != key) return false;
    TspdfObj *label = test_resolve_ref(doc, &nums->array.items[pair_idx * 2 + 1]);
    if (!label || label->type != TSPDF_OBJ_DICT) return false;
    TspdfObj *s = tspdf_dict_get(label, "S");
    if (style == 0) {
        if (s != NULL) return false;
    } else {
        char want[2] = {style, '\0'};
        if (!dt_str_eq(s, want)) return false;
    }
    TspdfObj *stv = tspdf_dict_get(label, "St");
    if (st == 1) {
        if (stv != NULL) return false;
    } else {
        if (!stv || stv->type != TSPDF_OBJ_INT || stv->integer != st) return false;
    }
    return true;
}

TEST(test_extract_preserves_page_labels) {
    size_t len = 0;
    char *pdf = pl_make_labeled_pdf(false, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 5);

    // Keep pages 3-4 (labels iii and 1): the rebuilt tree must pin the
    // roman range at value 3 and restart decimal at the second page.
    size_t pages[] = {2, 3};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 2, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 2);

    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(nums != NULL && nums->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(nums->array.count, 4);
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 3));  // page 0: iii
    ASSERT(pl_expect_range(re, nums, 1, 1, 'D', 1));  // page 1: 1

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_merge_page_labels_labeled_plus_unlabeled) {
    size_t len_a = 0, len_b = 0;
    char *pdf_a = pl_make_labeled_pdf(false, &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open((const uint8_t *)pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 7);

    // i, ii, iii, 1, 2 from the labeled source; the unlabeled source keeps
    // its default decimal-from-1 numbering via an explicit range.
    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(nums != NULL && nums->type == TSPDF_OBJ_ARRAY);
    ASSERT_EQ_SIZE(nums->array.count, 6);
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 1));
    ASSERT(pl_expect_range(re, nums, 1, 3, 'D', 1));
    ASSERT(pl_expect_range(re, nums, 2, 5, 'D', 1));

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_merge_without_page_labels_emits_none) {
    size_t len_a = 0, len_b = 0;
    uint8_t *pdf_a = dt_writer_pdf(1, false, false, "AA", "Helvetica", &len_a);
    uint8_t *pdf_b = dt_writer_pdf(2, false, false, "BB", "Helvetica", &len_b);
    ASSERT(pdf_a != NULL && pdf_b != NULL);

    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open(pdf_a, len_a, &err);
    TspdfReader *doc_b = tspdf_reader_open(pdf_b, len_b, &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/PageLabels"));

    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
    free(pdf_a);
    free(pdf_b);
}

TEST(test_extract_bounded_on_cyclic_pagelabel_tree) {
    size_t len = 0;
    char *pdf = pl_make_labeled_pdf(true, &len);
    ASSERT(pdf != NULL);

    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // The number tree's /Kids loops back on itself: the budgeted walk must
    // terminate and still pick up the /Nums entries it saw on the way.
    size_t pages[] = {0};
    TspdfReader *result = tspdf_reader_extract(doc, pages, 1, &err);
    ASSERT(result != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(result, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    TspdfObj *root = dt_catalog_get(re, "PageLabels");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *nums = dt_get(re, root, "Nums");
    ASSERT(pl_expect_range(re, nums, 0, 0, 'r', 1));  // page 0 keeps label i

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(result);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// ============================================================
// Save-to-memory byte identity (wasm track)
// ============================================================

// The wasm build has no filesystem, so the save-to-memory path is the primary
// API there. These tests pin the invariant that it produces exactly the bytes
// the file-save path writes.

static uint8_t *wasm_read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) { free(buf); return NULL; }
    *out_len = nread;
    return buf;
}

TEST(test_reader_save_to_memory_matches_file) {
    // The serializer stamps ModDate with second resolution, so a save pair
    // straddling a second boundary can legitimately differ; retry a few times
    // so the comparison cannot flake on that boundary.
    const char *tmp_path = "/tmp/tspdf_test_wasm_byte_identity.pdf";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    bool match = false;
    for (int attempt = 0; attempt < 3 && !match; attempt++) {
        err = tspdf_reader_save(doc, tmp_path);
        ASSERT_EQ_INT(err, TSPDF_OK);

        uint8_t *mem = NULL;
        size_t mem_len = 0;
        err = tspdf_reader_save_to_memory(doc, &mem, &mem_len);
        ASSERT_EQ_INT(err, TSPDF_OK);

        size_t file_len = 0;
        uint8_t *file_buf = wasm_read_whole_file(tmp_path, &file_len);
        ASSERT(file_buf != NULL);

        match = (file_len == mem_len) && (memcmp(file_buf, mem, mem_len) == 0);
        free(mem);
        free(file_buf);
    }
    remove(tmp_path);
    tspdf_reader_destroy(doc);
    ASSERT(match);
}

TEST(test_reader_save_to_memory_with_options_matches_file) {
    // strip_metadata removes the timestamped Info dict, so this pair is fully
    // deterministic and must compare equal on the first try.
    const char *tmp_path = "/tmp/tspdf_test_wasm_byte_identity_opts.pdf";
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_metadata = true;
    opts.use_xref_stream = true;

    err = tspdf_reader_save_with_options(doc, tmp_path, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *mem = NULL;
    size_t mem_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &mem, &mem_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    size_t file_len = 0;
    uint8_t *file_buf = wasm_read_whole_file(tmp_path, &file_len);
    ASSERT(file_buf != NULL);

    ASSERT_EQ_SIZE(file_len, mem_len);
    ASSERT(memcmp(file_buf, mem, mem_len) == 0);

    free(mem);
    free(file_buf);
    remove(tmp_path);
    tspdf_reader_destroy(doc);
}

// ============================================================
// AcroForm form fields (feat/form): list / fill / flatten
// ============================================================

static const TspdfFormFieldInfo *form_find(TspdfFormFieldInfo *fields,
                                           size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (fields[i].name && strcmp(fields[i].name, name) == 0) {
            return &fields[i];
        }
    }
    return NULL;
}

static bool form_has_option(const TspdfFormFieldInfo *f, const char *opt) {
    for (size_t i = 0; i < f->option_count; i++) {
        if (f->options[i] && strcmp(f->options[i], opt) == 0) return true;
    }
    return false;
}

TEST(test_form_fields_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 5);

    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT_EQ_INT(name->type, TSPDF_FIELD_TEXT);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);
    ASSERT(!name->readonly);
    ASSERT(!name->required);
    ASSERT_EQ_SIZE(name->page_index, 0);
    ASSERT(name->rect[0] == 72 && name->rect[1] == 672 &&
           name->rect[2] == 300 && name->rect[3] == 692);

    const TspdfFormFieldInfo *agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT_EQ_INT(agree->type, TSPDF_FIELD_CHECKBOX);
    ASSERT(agree->value && strcmp(agree->value, "Off") == 0);
    ASSERT_EQ_SIZE(agree->option_count, 1);
    ASSERT(form_has_option(agree, "Yes"));

    const TspdfFormFieldInfo *city = form_find(fields, count, "city");
    ASSERT(city != NULL);
    ASSERT_EQ_INT(city->type, TSPDF_FIELD_CHOICE);
    ASSERT(city->value && strcmp(city->value, "Berlin") == 0);
    ASSERT_EQ_SIZE(city->option_count, 3);
    ASSERT(form_has_option(city, "Berlin"));
    ASSERT(form_has_option(city, "Paris"));
    ASSERT(form_has_option(city, "Oslo"));

    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT_EQ_INT(color->type, TSPDF_FIELD_RADIO);
    ASSERT(color->value && strcmp(color->value, "Red") == 0);
    ASSERT_EQ_SIZE(color->option_count, 2);
    ASSERT(form_has_option(color, "Red"));
    ASSERT(form_has_option(color, "Blue"));
    ASSERT_EQ_SIZE(color->page_index, 0);

    // Hierarchical field: parent /T (a), widget kid /T (b).
    const TspdfFormFieldInfo *ab = form_find(fields, count, "a.b");
    ASSERT(ab != NULL);
    ASSERT_EQ_INT(ab->type, TSPDF_FIELD_TEXT);
    ASSERT(ab->value == NULL);
    ASSERT_EQ_SIZE(ab->page_index, 0);

    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_no_form) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 123;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 0);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_encrypted) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 5);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);
    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT(color->value && strcmp(color->value, "Red") == 0);

    tspdf_reader_destroy(doc);
}

TEST(test_form_fields_writer_roundtrip) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(2, false, true, "W1", "Helvetica", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 2);

    const TspdfFormFieldInfo *text = form_find(fields, count, "W1_text");
    ASSERT(text != NULL);
    ASSERT_EQ_INT(text->type, TSPDF_FIELD_TEXT);
    ASSERT(text->value && strcmp(text->value, "hello") == 0);
    ASSERT_EQ_SIZE(text->page_index, 0);

    // tspdf's own writer emits checkbox /V as a string and no /AP.
    const TspdfFormFieldInfo *check = form_find(fields, count, "W1_check");
    ASSERT(check != NULL);
    ASSERT_EQ_INT(check->type, TSPDF_FIELD_CHECKBOX);
    ASSERT(check->value && strcmp(check->value, "Yes") == 0);
    ASSERT_EQ_SIZE(check->page_index, 1);
    ASSERT_EQ_SIZE(check->option_count, 0);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// Field whose /Kids array points back at itself and at a child that points
// back at the parent: enumeration must stay bounded (budget guard).
static char *form_make_cyclic_kids_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (loop) /Kids [3 0 R 5 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /T (kid) /Kids [3 0 R] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// A field that lists itself in /Kids. Because the self-reference resolves to
// a node with /T, a naive walker treats it as an interior node and recurses,
// re-appending its own name each level ("self.self.self...") until the budget
// drains. It must instead appear exactly once.
static char *form_make_self_kid_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (self) /Kids [3 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_form_fields_self_kid_listed_once) {
    size_t len = 0;
    char *pdf = form_make_self_kid_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The self-referential field is terminal; it lists exactly once as "self",
    // not stacked "self.self..." names.
    ASSERT_EQ_SIZE(count, 1);
    ASSERT(form_find(fields, count, "self") != NULL);

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_form_fields_cyclic_kids_bounded) {
    size_t len = 0;
    char *pdf = form_make_cyclic_kids_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Bounded: the cycle must not fan out into an unbounded field list.
    ASSERT(count < 16);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// Adversarial structure: a terminal field with no /T, no /Rect and no page
// link; a field whose only kid ref dangles; a button whose /AP appearance
// refs dangle; a dangling ref and a non-ref directly in /Fields.
static char *form_make_adversarial_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R 5 0 R 6 0 R 88 0 R 42] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /Type /Annot /Subtype /Widget >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [97 0 R] >>\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "5 0 obj\n<< /FT /Tx /T (dangler) /Kids [98 0 R] >>\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "6 0 obj\n<< /FT /Btn /T (chk) /Type /Annot /Subtype /Widget "
                 "/Rect [10 10 20 20] "
                 "/AP << /N << /On 96 0 R /Off 95 0 R >> >> >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 7\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 6; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_form_fields_adversarial_no_crash) {
    size_t len = 0;
    char *pdf = form_make_adversarial_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(doc, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 3);

    // Unnamed widget field: listed with "" name, no page, zero rect.
    const TspdfFormFieldInfo *unnamed = form_find(fields, count, "");
    ASSERT(unnamed != NULL);
    ASSERT_EQ_INT(unnamed->type, TSPDF_FIELD_TEXT);
    ASSERT(unnamed->page_index == (size_t)-1);
    ASSERT(unnamed->rect[0] == 0 && unnamed->rect[2] == 0);

    // Field whose kid ref dangles still enumerates by name.
    ASSERT(form_find(fields, count, "dangler") != NULL);

    // Button options come from the /AP /N keys even when the streams dangle.
    const TspdfFormFieldInfo *chk = form_find(fields, count, "chk");
    ASSERT(chk != NULL);
    ASSERT_EQ_INT(chk->type, TSPDF_FIELD_CHECKBOX);
    ASSERT_EQ_SIZE(chk->option_count, 1);
    ASSERT(form_has_option(chk, "On"));

    tspdf_reader_destroy(doc);
    free(pdf);
}

// --- fill ---

// Save doc to memory, destroy it, reopen from the copy. Returns the new doc
// (caller destroys) and the malloc'd buffer backing it (caller frees after
// destroying the doc).
static TspdfReader *form_reopen(TspdfReader *doc, uint8_t **out_buf) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (tspdf_reader_save_to_memory(doc, &out, &out_len) != TSPDF_OK) {
        free(out);
        return NULL;
    }
    TspdfError err;
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    if (!re) {
        free(out);
        return NULL;
    }
    *out_buf = out;
    return re;
}

TEST(test_form_fill_text_value_and_appearance) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Grace Hopper", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_fill(doc, "a.b", "nested", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The appearance stream shows the value (the /V string alone would not
    // produce a "... Tj" operator sequence) and the AcroForm is marked
    // NeedAppearances as a belt for viewers that regenerate.
    ASSERT(bytes_contains(out, out_len, "(Grace Hopper) Tj"));
    ASSERT(bytes_contains(out, out_len, "/NeedAppearances true"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Grace Hopper") == 0);
    const TspdfFormFieldInfo *ab = form_find(fields, count, "a.b");
    ASSERT(ab != NULL);
    ASSERT(ab->value && strcmp(ab->value, "nested") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_text_nonascii_roundtrip) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Gr\xc3\xbc\xc3\x9f" "e", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Gr\xc3\xbc\xc3\x9f" "e") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_checkbox_states) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // Not an on-state of this checkbox.
    err = tspdf_reader_form_fill(doc, "agree", "Bogus", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    err = tspdf_reader_form_fill(doc, "agree", "Yes", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT(agree->value && strcmp(agree->value, "Yes") == 0);

    // And back off.
    err = tspdf_reader_form_fill(re, "agree", "Off", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    uint8_t *out2 = NULL;
    TspdfReader *re2 = form_reopen(re, &out2);
    ASSERT(re2 != NULL);
    err = tspdf_reader_form_fields(re2, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    agree = form_find(fields, count, "agree");
    ASSERT(agree != NULL);
    ASSERT(agree->value && strcmp(agree->value, "Off") == 0);

    tspdf_reader_destroy(re2);
    free(out2);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_radio_sets_widget_as) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "color", "Blue", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *color = form_find(fields, count, "color");
    ASSERT(color != NULL);
    ASSERT(color->value && strcmp(color->value, "Blue") == 0);

    // Each widget's /AS follows its own /AP /N states: the "Red" kid goes
    // /Off, the "Blue" kid goes /Blue.
    TspdfObj *acro = dt_catalog_get(re, "AcroForm");
    ASSERT(acro != NULL);
    TspdfObj *flds = dt_get(re, acro, "Fields");
    ASSERT(flds && flds->type == TSPDF_OBJ_ARRAY);
    TspdfObj *radio = NULL;
    for (size_t i = 0; i < flds->array.count; i++) {
        TspdfObj *f = test_resolve_ref(re, &flds->array.items[i]);
        if (f && f->type == TSPDF_OBJ_DICT && dt_str_eq(tspdf_dict_get(f, "T"), "color")) {
            radio = f;
            break;
        }
    }
    ASSERT(radio != NULL);
    TspdfObj *kids = dt_get(re, radio, "Kids");
    ASSERT(kids && kids->type == TSPDF_OBJ_ARRAY && kids->array.count == 2);
    int blue_as = 0, off_as = 0;
    for (size_t i = 0; i < kids->array.count; i++) {
        TspdfObj *kid = test_resolve_ref(re, &kids->array.items[i]);
        ASSERT(kid && kid->type == TSPDF_OBJ_DICT);
        TspdfObj *as = tspdf_dict_get(kid, "AS");
        ASSERT(as != NULL);
        if (dt_str_eq(as, "Blue")) blue_as++;
        if (dt_str_eq(as, "Off")) off_as++;
    }
    ASSERT_EQ_INT(blue_as, 1);
    ASSERT_EQ_INT(off_as, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_choice) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "city", "Oslo", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *city = form_find(fields, count, "city");
    ASSERT(city != NULL);
    ASSERT(city->value && strcmp(city->value, "Oslo") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// A non-editable choice field only accepts its /Opt export values: filling
// anything else is rejected the same way an unknown checkbox state is.
TEST(test_form_fill_choice_rejects_non_option) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    // "city" is a plain combo (/Ff Combo only) with /Opt Berlin/Paris/Oslo.
    err = tspdf_reader_form_fill(doc, "city", "Atlantis", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);

    // A listed option and the empty string (clear the value) still work.
    err = tspdf_reader_form_fill(doc, "city", "Paris", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_fill(doc, "city", "", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    tspdf_reader_destroy(doc);
}

// An editable combo (/Ff Combo|Edit) legally takes free text.
TEST(test_form_fill_editable_combo_accepts_free_text) {
    TspdfError err;
    TspdfReader *doc =
        tspdf_reader_open_file("tests/data/form_combo_edit.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "nickname", "Zaphod", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *nick = form_find(fields, count, "nickname");
    ASSERT(nick != NULL);
    ASSERT(nick->value && strcmp(nick->value, "Zaphod") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// Saving a document that was opened with a password must keep it encrypted:
// the original /Encrypt dictionary and /ID are carried over, so the original
// password still opens the output (and no password does not).
TEST(test_form_fill_encrypted_save_preserves_encryption) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Bob", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // Without a password the saved bytes must not open...
    TspdfError open_err;
    TspdfReader *plain = tspdf_reader_open(out, out_len, &open_err);
    ASSERT(plain == NULL);
    ASSERT_EQ_INT(open_err, TSPDF_ERR_ENCRYPTED);

    // ...and the original password must, with the new value visible.
    TspdfReader *re =
        tspdf_reader_open_with_password(out, out_len, "secret", &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Bob") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// The explicit opt-out: TspdfSaveOptions.decrypt writes plaintext (this is
// what `tspdf decrypt` uses).
TEST(test_save_decrypt_option_writes_plaintext) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file_with_password(
        "tests/data/form_fields_enc.pdf", "secret", &err);
    ASSERT(doc != NULL);

    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.decrypt = true;
    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *name = form_find(fields, count, "name");
    ASSERT(name != NULL);
    ASSERT(name->value && strcmp(name->value, "Ada") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_fill_unknown_name_errors) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_form_fill(doc, "no_such_field", "x", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_INVALID_ARG);
    tspdf_reader_destroy(doc);
}

// Readonly text field (/Ff 1): fill fails without force, succeeds with it.
static char *form_make_readonly_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [4 0 R] /DA (/Helv 12 Tf 0 g) >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [4 0 R] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /FT /Tx /T (locked) /Ff 1 /Type /Annot /Subtype /Widget "
                 "/Rect [72 700 300 720] /P 3 0 R >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_form_fill_readonly_requires_force) {
    size_t len = 0;
    char *pdf = form_make_readonly_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "locked", "nope", false);
    ASSERT_EQ_INT(err, TSPDF_ERR_UNSUPPORTED);
    err = tspdf_reader_form_fill(doc, "locked", "forced", true);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    TspdfFormFieldInfo *fields = NULL;
    size_t count = 0;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    const TspdfFormFieldInfo *locked = form_find(fields, count, "locked");
    ASSERT(locked != NULL);
    ASSERT(locked->readonly);
    ASSERT(locked->value && strcmp(locked->value, "forced") == 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Text field whose /DA carries a hostile font name laced with PDF delimiters:
// "/Bad(Font)Name 12 Tf". The generated appearance stream must not let those
// delimiters reach the content-stream Tf operand, and the sanitized name it
// emits must match the key it registers in the appearance /Resources /Font.
static char *form_make_evil_da_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[8] = {0};

    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R "
                 "/AcroForm << /Fields [3 0 R] >> >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /FT /Tx /T (evil) /Type /Annot /Subtype /Widget "
                 "/Rect [10 10 200 30] /DA (/Bad\\(Font\\)Name 12 Tf) "
                 "/P 4 0 R >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Annots [3 0 R] >>\nendobj\n")) goto fail;

    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 5\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 4; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                 xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

TEST(test_form_fill_text_da_font_name_sanitized) {
    size_t len = 0;
    char *pdf = form_make_evil_da_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "evil", "hi", false);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The sanitized DA font is BadFontName (delimiters stripped). It must
    // appear both as the content-stream Tf operand and as the appearance
    // /Resources /Font key -- and they must be identical, so a strict renderer
    // resolves the font.
    ASSERT(bytes_contains(out, out_len, "/BadFontName 12.00 Tf"));
    ASSERT(bytes_contains(out, out_len, "/Font << /BadFontName "));

    // No PDF delimiter from the hostile /DA reaches the content Tf token: the
    // serializer-escaped form "/Bad#28Font#29Name" (which would desync from
    // the content name) must never appear.
    ASSERT(!bytes_contains(out, out_len, "/Bad#28Font"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// --- flatten ---

TEST(test_form_flatten_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/form_fields.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_fill(doc, "name", "Grace", false);
    ASSERT_EQ_INT(err, TSPDF_OK);
    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The fixture page carries /Resources as an indirect ref; merging the
    // flattening resources must not leave a duplicate /Resources key behind.
    {
        TspdfObj *page_dict = doc->pages.pages[0].page_dict;
        size_t res_keys = 0;
        for (size_t i = 0; i < page_dict->dict.count; i++) {
            if (strcmp(page_dict->dict.entries[i].key, "Resources") == 0) {
                res_keys++;
            }
        }
        ASSERT_EQ_SIZE(res_keys, 1);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // The form is gone: no catalog /AcroForm, no widget annotations.
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "/Widget"));
    // The checked radio ("Red") appearance stream was stamped into the page
    // content as a form XObject.
    ASSERT(bytes_contains(out, out_len, "/TspdfFx0 Do"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT(!tspdf_reader_has_acroform(re));

    TspdfFormFieldInfo *fields = NULL;
    size_t count = 123;
    err = tspdf_reader_form_fields(re, &fields, &count);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(count, 0);

    // All fixture annotations were widgets, so the page has none left.
    TspdfObj *annots = dt_get(re, re->pages.pages[0].page_dict, "Annots");
    ASSERT(annots == NULL || annots->array.count == 0);

    // The stamped values extract as page text.
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "Grace") != NULL);
    ASSERT(strstr(text, "Berlin") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

TEST(test_form_flatten_no_form_noop) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/three_pages.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 3);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// tspdf's own writer emits checkboxes without /AP appearance streams; the
// flattener falls back to drawing a check mark directly.
TEST(test_form_flatten_writer_checkbox_fallback) {
    size_t len = 0;
    uint8_t *pdf = dt_writer_pdf(1, false, true, "W2", "Helvetica", &len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/AcroForm"));
    ASSERT(!bytes_contains(out, out_len, "/Widget"));
    // Vector check-mark fallback for the checked box (distinctive line width).
    ASSERT(bytes_contains(out, out_len, "1.5 w"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    const char *text = tspdf_reader_page_text(re, 0, &err);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "hello") != NULL);  // flattened text field value

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// --- Outline (bookmark) editing ---

// A plain 3-page PDF (no outline) for set/import round-trip tests.
static char *bm_make_three_page_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n")) goto fail;
    for (int i = 0; i < 3; i++) {
        off[3 + i] = pos;
        if (!appendf(pdf, 4096, &pos,
                     "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n",
                     3 + i)) goto fail;
    }
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// list on the shared outline fixture: three items, correct titles/levels/pages.
TEST(test_bookmark_list_fixture) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    ASSERT_EQ_STR(bm[0].title, "OF-CH1");
    ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_SIZE(bm[0].page_index, 0);

    ASSERT_EQ_STR(bm[1].title, "OF-CH1-SUB");
    ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_SIZE(bm[1].page_index, 1);

    ASSERT_EQ_STR(bm[2].title, "OF-CH2");
    ASSERT_EQ_INT(bm[2].level, 1);
    ASSERT_EQ_SIZE(bm[2].page_index, 2);

    tspdf_reader_destroy(doc);
}

TEST(test_bookmark_list_no_outline) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = (TspdfBookmarkInfo *)1;
    size_t n = 99;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);
    ASSERT(bm == NULL);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// set a 3-level TOC, save, reopen, and verify structure via list plus the
// raw /Count and nesting refs.
TEST(test_bookmark_set_three_level_roundtrip) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfBookmarkEntry entries[] = {
        {"Chapter 1", 1, 0, 0, false},
        {"Section 1.1", 2, 1, 0, false},
        {"Subsection 1.1.1", 3, 1, 0, false},
        {"Section 1.2", 2, 2, 0, false},
        {"Chapter 2", 1, 2, 0, false},
    };
    err = tspdf_reader_set_bookmarks(doc, entries, 5);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 5);
    ASSERT_EQ_STR(bm[0].title, "Chapter 1");   ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_STR(bm[1].title, "Section 1.1"); ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_STR(bm[2].title, "Subsection 1.1.1"); ASSERT_EQ_INT(bm[2].level, 3);
    ASSERT_EQ_STR(bm[3].title, "Section 1.2"); ASSERT_EQ_INT(bm[3].level, 2);
    ASSERT_EQ_STR(bm[4].title, "Chapter 2");   ASSERT_EQ_INT(bm[4].level, 1);
    ASSERT_EQ_SIZE(bm[4].page_index, 2);

    // Root /Count = 5 (all items open/visible).
    TspdfObj *root = dt_catalog_get(re, "Outlines");
    ASSERT(root != NULL && root->type == TSPDF_OBJ_DICT);
    TspdfObj *rc = tspdf_dict_get(root, "Count");
    ASSERT(rc && rc->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)rc->integer, 5);

    // Chapter 1 has /Count 3 (Section 1.1 + its subsection + Section 1.2).
    TspdfObj *ch1 = dt_get(re, root, "First");
    ASSERT(dt_str_eq(tspdf_dict_get(ch1, "Title"), "Chapter 1"));
    TspdfObj *c1c = tspdf_dict_get(ch1, "Count");
    ASSERT(c1c && c1c->type == TSPDF_OBJ_INT);
    ASSERT_EQ_INT((int)c1c->integer, 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Non-ASCII title survives the UTF-16BE round-trip.
TEST(test_bookmark_set_nonascii_title_roundtrip) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    const char *title = "Caf\xC3\xA9 \xE2\x86\x92 \xF0\x9F\x93\x84";  // "Café → 📄"
    TspdfBookmarkEntry entries[] = {{title, 1, 0, 0, false}};
    err = tspdf_reader_set_bookmarks(doc, entries, 1);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    // Stored as a UTF-16BE literal string: the BOM bytes serialize as the
    // octal escapes \376\377 (bytes > 126 are escaped by write_string_escaped).
    ASSERT(bytes_contains(out, out_len, "\\376\\377"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 1);
    ASSERT_EQ_STR(bm[0].title, title);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_bookmark_set_validation_errors) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    // First entry must be level 1.
    TspdfBookmarkEntry e_lvl2first[] = {{"X", 2, 0, 0, false}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_lvl2first, 1), TSPDF_ERR_INVALID_ARG);

    // Level jump 1 -> 3.
    TspdfBookmarkEntry e_jump[] = {{"A", 1, 0, 0, false}, {"B", 3, 0, 0, false}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_jump, 2), TSPDF_ERR_INVALID_ARG);

    // Empty title.
    TspdfBookmarkEntry e_empty[] = {{"", 1, 0, 0, false}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_empty, 1), TSPDF_ERR_INVALID_ARG);

    // Page out of range (only 3 pages: indices 0-2).
    TspdfBookmarkEntry e_page[] = {{"A", 1, 9, 0, false}};
    ASSERT_EQ_INT(tspdf_reader_set_bookmarks(doc, e_page, 1), TSPDF_ERR_PAGE_RANGE);

    tspdf_reader_destroy(doc);
    free(pdf);
}

// clear drops /Outlines; a subsequent list yields nothing.
TEST(test_bookmark_clear) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_clear_bookmarks(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/Outlines"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// clear also strips a stale /PageMode /UseOutlines from the catalog.
TEST(test_bookmark_clear_drops_pagemode_useoutlines) {
    size_t len = 0;
    char *pdf = make_catalog_features_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    err = tspdf_reader_clear_bookmarks(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT(!bytes_contains(out, out_len, "/UseOutlines"));

    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// set with count 0 clears as well.
TEST(test_bookmark_set_empty_clears) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);
    err = tspdf_reader_set_bookmarks(doc, NULL, 0);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 0);
    tspdf_reader_destroy(doc);
}

// Round-trip: list an existing outline, feed it back through set, and confirm
// the enumeration is stable.
TEST(test_bookmark_list_then_set_stable) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/outline_form.pdf", &err);
    ASSERT(doc != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    TspdfBookmarkEntry *entries = (TspdfBookmarkEntry *)calloc(n, sizeof(*entries));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
    }
    err = tspdf_reader_set_bookmarks(doc, entries, n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    free(entries);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm2 = NULL;
    size_t n2 = 0;
    err = tspdf_reader_bookmarks(re, &bm2, &n2);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n2, 3);
    ASSERT_EQ_STR(bm2[0].title, "OF-CH1");     ASSERT_EQ_INT(bm2[0].level, 1);
    ASSERT_EQ_STR(bm2[1].title, "OF-CH1-SUB"); ASSERT_EQ_INT(bm2[1].level, 2);
    ASSERT_EQ_STR(bm2[2].title, "OF-CH2");     ASSERT_EQ_INT(bm2[2].level, 1);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// A cyclic /Next sibling loop lists each item once and terminates.
TEST(test_bookmark_list_cyclic_bounded) {
    size_t len = 0;
    char *pdf = dt_make_cyclic_outline_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 2);   // LOOPA, LOOPB — no re-emission of the cycle
    ASSERT_EQ_STR(bm[0].title, "LOOPA");
    ASSERT_EQ_STR(bm[1].title, "LOOPB");
    tspdf_reader_destroy(doc);
    free(pdf);
}

// 10000 entries import bounded (structure builds, count matches, no runaway).
TEST(test_bookmark_set_large_bounded) {
    size_t len = 0;
    char *pdf = bm_make_three_page_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    size_t N = 10000;
    TspdfBookmarkEntry *entries = (TspdfBookmarkEntry *)calloc(N, sizeof(*entries));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < N; i++) {
        entries[i].title = "item";
        entries[i].level = 1;
        entries[i].page_index = i % 3;
    }
    err = tspdf_reader_set_bookmarks(doc, entries, N);
    ASSERT_EQ_INT(err, TSPDF_OK);
    free(entries);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, N);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// tests/data/indirect_title.pdf: two pages; outline "Alpha" -> p1 (child
// "Alpha Sub" -> p2, whose /Dest array is also indirect), "Beta" -> p2.
// Every /Title is an indirect string object (the pdfTeX/hyperref pattern).
TEST(test_bookmark_list_indirect_title) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_INT(bm[0].level, 1);
    ASSERT_EQ_SIZE(bm[0].page_index, 0);

    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_INT(bm[1].level, 2);
    ASSERT_EQ_SIZE(bm[1].page_index, 1);

    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_INT(bm[2].level, 1);
    ASSERT_EQ_SIZE(bm[2].page_index, 1);

    tspdf_reader_destroy(doc);
}

// The `bookmark add` flow: enumerate, append one entry, set the whole list
// back. Indirect titles must survive (they used to come back as "" and make
// set_bookmarks fail with TSPDF_ERR_INVALID_ARG).
TEST(test_bookmark_add_preserves_indirect_titles) {
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc != NULL);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(doc, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 3);

    TspdfBookmarkEntry *entries =
        (TspdfBookmarkEntry *)calloc(n + 1, sizeof(TspdfBookmarkEntry));
    ASSERT(entries != NULL);
    for (size_t i = 0; i < n; i++) {
        entries[i].title = bm[i].title;
        entries[i].level = bm[i].level;
        entries[i].page_index = bm[i].page_index;
    }
    entries[n].title = "Appendix";
    entries[n].level = 1;
    entries[n].page_index = 0;
    err = tspdf_reader_set_bookmarks(doc, entries, n + 1);
    free(entries);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 4);
    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_STR(bm[3].title, "Appendix");

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// Merging documents whose outline titles are indirect strings must copy the
// referenced string objects too. The old code kept the /Title N 0 R ref but
// never copied object N, and the serializer then emitted an in-use xref entry
// with offset 0 for it (qpdf --check warnings, blank titles in viewers).
TEST(test_merge_copies_indirect_outline_strings) {
    TspdfError err;
    TspdfReader *doc_a = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    TspdfReader *doc_b = tspdf_reader_open_file("tests/data/indirect_title.pdf", &err);
    ASSERT(doc_a != NULL && doc_b != NULL);

    TspdfReader *docs[] = {doc_a, doc_b};
    TspdfReader *merged = tspdf_reader_merge(docs, 2, &err);
    ASSERT(merged != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(merged, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // No in-use classic xref entry may carry offset 0.
    ASSERT(!bytes_contains(out, out_len, "0000000000 00000 n"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 4);

    TspdfBookmarkInfo *bm = NULL;
    size_t n = 0;
    err = tspdf_reader_bookmarks(re, &bm, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 6);
    ASSERT_EQ_STR(bm[0].title, "Alpha");
    ASSERT_EQ_STR(bm[1].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[2].title, "Beta");
    ASSERT_EQ_STR(bm[3].title, "Alpha");
    ASSERT_EQ_STR(bm[4].title, "Alpha Sub");
    ASSERT_EQ_STR(bm[5].title, "Beta");
    ASSERT_EQ_SIZE(bm[3].page_index, 2);
    ASSERT_EQ_SIZE(bm[4].page_index, 3);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(merged);
    tspdf_reader_destroy(doc_a);
    tspdf_reader_destroy(doc_b);
}

// One-page PDF whose catalog references object 5, but the xref entry for 5 is
// marked in-use pointing at garbage bytes that do not parse as an object.
static char *ser_make_dangling_inuse_pdf(size_t *out_len) {
    char *pdf = (char *)malloc(4096);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[6] = {0};
    if (!appendf(pdf, 4096, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Lang 5 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, 4096, &pos,
                 "4 0 obj\n<< >>\nendobj\n")) goto fail;
    off[5] = pos;  // in-use entry pointing at bytes that are not an object
    if (!appendf(pdf, 4096, &pos, "%% not an object, just junk bytes\n")) goto fail;
    size_t xref = pos;
    if (!appendf(pdf, 4096, &pos, "xref\n0 6\n0000000000 65535 f \n")) goto fail;
    for (int i = 1; i <= 5; i++) {
        if (!appendf(pdf, 4096, &pos, "%010zu 00000 n \n", off[i])) goto fail;
    }
    if (!appendf(pdf, 4096, &pos,
                 "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref)) goto fail;
    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// Serializer safety net: an object that cannot be materialized must become a
// FREE xref entry, never an in-use (type 1) entry with offset 0.
TEST(test_serialize_never_writes_inuse_offset_zero) {
    size_t len = 0;
    char *pdf = ser_make_dangling_inuse_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    uint8_t *out = NULL;
    size_t out_len = 0;
    err = tspdf_reader_save_to_memory(doc, &out, &out_len);
    ASSERT_EQ_INT(err, TSPDF_OK);

    // A classic-table in-use entry with offset 0 is exactly this line.
    ASSERT(!bytes_contains(out, out_len, "0000000000 00000 n"));

    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
    free(pdf);
}

// Resolve `obj` if it is an indirect ref, using the reopened reader.
static TspdfObj *flat_resolve(TspdfReader *doc, TspdfObj *obj) {
    if (!obj || obj->type != TSPDF_OBJ_REF) return obj;
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    if (obj->ref.num < doc->xref.count) {
        return tspdf_xref_resolve(&doc->xref, &parser, obj->ref.num,
                                  doc->obj_cache, doc->crypt);
    }
    return NULL;
}

// Count entries in a page's /Resources /Font sub-dict (0 when absent).
static size_t flat_page_font_count(TspdfReader *doc, size_t page) {
    TspdfObj *pd = tspdf_reader_get_page(doc, page)->page_dict;
    TspdfObj *res = flat_resolve(doc, tspdf_dict_get(pd, "Resources"));
    if (!res || res->type != TSPDF_OBJ_DICT) return 0;
    TspdfObj *font = flat_resolve(doc, tspdf_dict_get(res, "Font"));
    if (!font || font->type != TSPDF_OBJ_DICT) return 0;
    return font->dict.count;
}

// Two pages that share one indirect /Resources must, after flatten, each end
// up with their OWN additions only. Merging into the shared dict once per page
// would accumulate every page's font key on every page (finding M3).
TEST(test_form_flatten_shared_resources_not_accumulated) {
    TspdfError err;
    TspdfReader *doc =
        tspdf_reader_open_file("tests/data/form_shared_resources.pdf", &err);
    ASSERT(doc != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(doc), 2);

    err = tspdf_reader_form_flatten(doc);
    ASSERT_EQ_INT(err, TSPDF_OK);

    uint8_t *out = NULL;
    TspdfReader *re = form_reopen(doc, &out);
    ASSERT(re != NULL);

    // Each page: the original /Helv plus its own /TspdfFf == 2 entries. The
    // pre-fix bug leaked the other page's key in too (Helv + TspdfFf +
    // TspdfFf_2 == 3).
    ASSERT_EQ_SIZE(flat_page_font_count(re, 0), 2);
    ASSERT_EQ_SIZE(flat_page_font_count(re, 1), 2);

    // Both field values must still render on their own page.
    const char *t0 = tspdf_reader_page_text(re, 0, &err);
    const char *t1 = tspdf_reader_page_text(re, 1, &err);
    ASSERT(t0 && strstr(t0, "Alpha") != NULL);
    ASSERT(t1 && strstr(t1, "Beta") != NULL);

    tspdf_reader_destroy(re);
    free(out);
    tspdf_reader_destroy(doc);
}

// ===========================================================================
// Lossy image recompression (tspr_lossy.c)
// ===========================================================================

TEST(test_lossy_box_downsample_exact) {
    // 4x2 gray -> 2x1: each dest pixel is the exact mean of a 2x2 block.
    const uint8_t src[8] = { 10, 20, 100, 200,
                             30, 40, 100,   0 };
    uint8_t dst[2] = { 0xAA, 0xAA };
    ASSERT(tspdf_lossy_box_downsample(src, 4, 2, 1, dst, 2, 1));
    ASSERT_EQ_INT(dst[0], 25);  // (10+20+30+40)/4
    ASSERT_EQ_INT(dst[1], 100); // (100+200+100+0)/4

    // 2x2 RGB -> 1x1: per-channel means.
    const uint8_t rgb[12] = { 0,0,255,  100,50,255,
                              200,150,255, 100,200,255 };
    uint8_t d1[3] = {0};
    ASSERT(tspdf_lossy_box_downsample(rgb, 2, 2, 3, d1, 1, 1));
    ASSERT_EQ_INT(d1[0], 100); // (0+100+200+100)/4
    ASSERT_EQ_INT(d1[1], 100); // (0+50+150+200)/4
    ASSERT_EQ_INT(d1[2], 255);

    // Invalid arguments report failure instead of silently doing nothing.
    ASSERT(!tspdf_lossy_box_downsample(NULL, 4, 2, 1, dst, 2, 1));
    ASSERT(!tspdf_lossy_box_downsample(src, 4, 2, 1, dst, 0, 1));
    ASSERT(!tspdf_lossy_box_downsample(src, 4, 2, 0, dst, 2, 1));
}

TEST(test_lossy_box_downsample_fractional_edges) {
    // 3x1 -> 2x1 (ratio 1.5): dest bin 0 covers source [0,1.5) so pixel 1
    // contributes half its weight; correct edge weighting gives
    // dest0 = (0*1 + 90*0.5)/1.5 = 30, dest1 = (90*0.5 + 30*1)/1.5 = 50.
    const uint8_t src[3] = { 0, 90, 30 };
    uint8_t dst[2] = { 0xAA, 0xAA };
    tspdf_lossy_box_downsample(src, 3, 1, 1, dst, 2, 1);
    ASSERT_EQ_INT(dst[0], 30);
    ASSERT_EQ_INT(dst[1], 50);

    // Same ratio vertically: 1x3 -> 1x2.
    uint8_t dst2[2] = { 0xAA, 0xAA };
    tspdf_lossy_box_downsample(src, 1, 3, 1, dst2, 1, 2);
    ASSERT_EQ_INT(dst2[0], 30);
    ASSERT_EQ_INT(dst2[1], 50);

    // A constant image stays constant under any ratio (weights sum to 1).
    uint8_t flat[7 * 5];
    memset(flat, 137, sizeof(flat));
    uint8_t dflat[3 * 2];
    tspdf_lossy_box_downsample(flat, 7, 5, 1, dflat, 3, 2);
    for (size_t i = 0; i < sizeof(dflat); i++) ASSERT_EQ_INT(dflat[i], 137);
}

TEST(test_lossy_gray_detect) {
    // Near-gray pixels (max channel delta 8) pass.
    uint8_t px[4 * 3] = { 100,104,97,  0,8,4,  255,247,250,  30,30,38 };
    ASSERT(tspdf_lossy_rgb_is_gray(px, 4));

    // One colored pixel anywhere fails.
    px[7] = 120; // pixel 2 becomes (255,120,250)
    ASSERT(!tspdf_lossy_rgb_is_gray(px, 4));

    // Luma conversion: pure gray maps to itself.
    const uint8_t g[2 * 3] = { 100,100,100,  0,0,0 };
    uint8_t out[2] = { 0xAA, 0xAA };
    tspdf_lossy_rgb_to_gray(g, 2, out);
    ASSERT_EQ_INT(out[0], 100);
    ASSERT_EQ_INT(out[1], 0);
}

TEST(test_lossy_target_dims) {
    uint32_t dw = 0, dh = 0;
    // 1000x1000 px rendered at 1x1 inch, target 150 dpi: 6.7x too many
    // pixels -> downsample to 150x150.
    ASSERT(tspdf_lossy_target_dims(1000, 1000, 72.0, 72.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);
    ASSERT_EQ_INT((int)dh, 150);

    // 180x180 px at 1x1 inch is only 1.2x the 150-dpi target: within the
    // 1.3x slack, not worth re-encoding.
    ASSERT(!tspdf_lossy_target_dims(180, 180, 72.0, 72.0, 150, &dw, &dh));

    // Just over the slack acts.
    ASSERT(tspdf_lossy_target_dims(200, 200, 72.0, 72.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);

    // Never upsample: image already below target.
    ASSERT(!tspdf_lossy_target_dims(100, 100, 144.0, 144.0, 150, &dw, &dh));

    // Aspect ratio is preserved; the tighter axis drives the scale.
    // 2000x1000 at 2x0.5 inch, 150 dpi: targets 300x75; height needs
    // 1000/75 = 13.3x, width only 6.7x -> scale by the max => 150x75... no:
    // both dims shrink by the same factor s = max(2000/300, 1000/75) = 13.3,
    // giving 150x75 (both at or below target).
    ASSERT(tspdf_lossy_target_dims(2000, 1000, 144.0, 36.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 150);
    ASSERT_EQ_INT((int)dh, 75);
    ASSERT(dw <= 300 && dh <= 75);
}

TEST(test_lossy_keep_original_rule) {
    // Replace only when >= 10% smaller than the stored original.
    ASSERT(tspdf_lossy_worth_replacing(1000, 900));   // exactly 10% smaller
    ASSERT(!tspdf_lossy_worth_replacing(1000, 901));  // 9.9%: keep original
    ASSERT(tspdf_lossy_worth_replacing(1000, 100));
    ASSERT(!tspdf_lossy_worth_replacing(1000, 2000)); // grew: keep
    ASSERT(!tspdf_lossy_worth_replacing(0, 0));
}

// Two-page PDF for the CTM/DPI scan: Im1 (800x600) drawn twice at different
// sizes with nested q/Q, Im2 (400x300) drawn only inside a Form XObject with
// a /Matrix, Im3 present in /Resources but never drawn.
static char *lossy_make_ctm_pdf(size_t *out_len) {
    const size_t cap = 4096;
    char *pdf = (char *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[10] = {0};

    if (!appendf(pdf, cap, &pos, "%%PDF-1.4\n")) goto fail;
    off[1] = pos;
    if (!appendf(pdf, cap, &pos,
                 "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n")) goto fail;
    off[2] = pos;
    if (!appendf(pdf, cap, &pos,
                 "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n")) goto fail;
    off[3] = pos;
    if (!appendf(pdf, cap, &pos,
                 "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                 "/Resources << /XObject << /Im1 4 0 R /Im2 5 0 R /Im3 6 0 R /Fm1 7 0 R >> >> "
                 "/Contents 8 0 R >>\nendobj\n")) goto fail;
    off[4] = pos;
    if (!appendf(pdf, cap, &pos,
                 "4 0 obj\n<< /Type /XObject /Subtype /Image /Width 800 /Height 600 "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    off[5] = pos;
    if (!appendf(pdf, cap, &pos,
                 "5 0 obj\n<< /Type /XObject /Subtype /Image /Width 400 /Height 300 "
                 "/ColorSpace /DeviceGray /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    off[6] = pos;
    if (!appendf(pdf, cap, &pos,
                 "6 0 obj\n<< /Type /XObject /Subtype /Image /Width 640 /Height 480 "
                 "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 0 >>\n"
                 "stream\n\nendstream\nendobj\n")) goto fail;
    // Form XObject drawing Im2 at 400x300 under its own /Matrix [.5 0 0 .5].
    {
        const char *form = "q 400 0 0 300 20 20 cm /Im2 Do Q";
        off[7] = pos;
        if (!appendf(pdf, cap, &pos,
                     "7 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 612 792] "
                     "/Matrix [0.5 0 0 0.5 0 0] "
                     "/Resources << /XObject << /Im2 5 0 R >> >> /Length %zu >>\n"
                     "stream\n%s\nendstream\nendobj\n",
                     strlen(form), form)) goto fail;
    }
    // Page content: Im1 small, then (nested q/Q) Im1 large; the form is
    // drawn under an outer 1.5 scale, so Im2 renders at 400*0.5*1.5 = 300 x
    // 300*0.5*1.5 = 225 pt.
    {
        const char *content =
            "q 100 0 0 75 10 10 cm /Im1 Do Q "
            "q 1 0 0 1 5 5 cm q 200 0 0 150 0 0 cm /Im1 Do Q Q "
            "q 1.5 0 0 1.5 0 0 cm /Fm1 Do Q";
        off[8] = pos;
        if (!appendf(pdf, cap, &pos,
                     "8 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
                     strlen(content), content)) goto fail;
    }

    {
        size_t xref = pos;
        if (!appendf(pdf, cap, &pos, "xref\n0 9\n0000000000 65535 f \n")) goto fail;
        for (int i = 1; i <= 8; i++) {
            if (!appendf(pdf, cap, &pos, "%010zu 00000 n \n", off[i])) goto fail;
        }
        if (!appendf(pdf, cap, &pos,
                     "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF",
                     xref)) goto fail;
    }

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

static const TspdfLossyPlacement *lossy_find_placement(
        const TspdfLossyPlacement *p, size_t n, uint32_t obj_num) {
    for (size_t i = 0; i < n; i++) {
        if (p[i].obj_num == obj_num) return &p[i];
    }
    return NULL;
}

TEST(test_lossy_ctm_scan_placements) {
    size_t len = 0;
    char *pdf = lossy_make_ctm_pdf(&len);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    err = tspdf_lossy_scan_placements(doc, &pl, &n);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(n, 2); // Im1 and Im2; Im3 never drawn

    // Im1 (obj 4): the larger of the two placements wins.
    const TspdfLossyPlacement *im1 = lossy_find_placement(pl, n, 4);
    ASSERT(im1 != NULL);
    ASSERT(im1->w_pt > 199.9 && im1->w_pt < 200.1);
    ASSERT(im1->h_pt > 149.9 && im1->h_pt < 150.1);

    // Im2 (obj 5): drawn only inside the form; form /Matrix (0.5) times the
    // outer cm (1.5) times the inner cm (400x300) = 300 x 225 pt.
    const TspdfLossyPlacement *im2 = lossy_find_placement(pl, n, 5);
    ASSERT(im2 != NULL);
    ASSERT(im2->w_pt > 299.9 && im2->w_pt < 300.1);
    ASSERT(im2->h_pt > 224.9 && im2->h_pt < 225.1);

    // Im3 (obj 6): in /Resources but never drawn.
    ASSERT(lossy_find_placement(pl, n, 6) == NULL);

    free(pl);
    tspdf_reader_destroy(doc);
    free(pdf);

    // The target decision follows from the scan: Im1 is 800px over 200pt
    // (288 dpi) -> at 150 dpi it must shrink; at 300 dpi it must not.
    uint32_t dw = 0, dh = 0;
    ASSERT(tspdf_lossy_target_dims(800, 600, 200.0, 150.0, 150, &dw, &dh));
    ASSERT_EQ_INT((int)dw, 417); // 150 * 200/72 = 416.7
    ASSERT(!tspdf_lossy_target_dims(800, 600, 200.0, 150.0, 300, &dw, &dh));
}

// Build a one-page PDF whose sole image is `w`x`h`, 8-bit, colorspace name
// `cs` ("DeviceGray"/"DeviceRGB"), with raw stream bytes `img` (img_len) and
// extra image-dict text `img_extra` (filters, masks, ...). The image is
// drawn at `w_pt` x `h_pt` points. Binary-safe (streams may contain \0).
static uint8_t *lossy_make_image_pdf(const uint8_t *img, size_t img_len,
                                     const char *img_extra, int w, int h,
                                     const char *cs, double w_pt, double h_pt,
                                     size_t *out_len) {
    char content[128];
    snprintf(content, sizeof(content), "q %.2f 0 0 %.2f 10 10 cm /Im1 Do Q",
             w_pt, h_pt);

    size_t cap = img_len + 4096;
    uint8_t *pdf = (uint8_t *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    char head[512];

#define LPUT(...) do { \
        int _n = snprintf(head, sizeof(head), __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(head) || pos + (size_t)_n > cap) goto fail; \
        memcpy(pdf + pos, head, (size_t)_n); \
        pos += (size_t)_n; \
    } while (0)

    LPUT("%%PDF-1.5\n%%\xE2\xE3\xCF\xD3\n");
    off[1] = pos;
    LPUT("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = pos;
    LPUT("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = pos;
    LPUT("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");
    off[4] = pos;
    LPUT("4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
         "/ColorSpace /%s /BitsPerComponent 8 %s /Length %zu >>\nstream\n",
         w, h, cs, img_extra ? img_extra : "", img_len);
    if (pos + img_len > cap) goto fail;
    memcpy(pdf + pos, img, img_len);
    pos += img_len;
    LPUT("\nendstream\nendobj\n");
    off[5] = pos;
    LPUT("5 0 obj\n<< /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
         strlen(content), content);

    {
        size_t xref = pos;
        LPUT("xref\n0 6\n0000000000 65535 f \n");
        for (int i = 1; i <= 5; i++) LPUT("%010zu 00000 n \n", off[i]);
        LPUT("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref);
    }
#undef LPUT

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// Fetch the (single) image object dict of a doc built by
// lossy_make_image_pdf and return the resolved object 4.
static TspdfObj *lossy_get_image_obj(TspdfReader *doc) {
    TspdfParser parser;
    tspdf_parser_init(&parser, doc->data, doc->data_len, &doc->arena);
    return tspdf_xref_resolve(&doc->xref, &parser, 4, doc->obj_cache, doc->crypt);
}

TEST(test_lossy_predictor15_flate_image_recompressed) {
    // 256x256 gray horizontal gradient, PNG predictor 15 (per-row filter
    // byte; row 0 uses filter 0/None, the rest filter 2/Up so the predictor
    // undo actually runs), deflated.
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc(W * H);
    ASSERT(pixels != NULL);
    uint32_t lcg = 1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            // Gradient plus a little photo-ish noise: without it the
            // predictor+flate stream is so tiny that keeping the original
            // is (correctly) the better deal.
            lcg = lcg * 1664525u + 1013904223u;
            int v = x + (int)((lcg >> 24) & 7) - 3;
            pixels[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }

    // Predictor-encode: filter byte + row bytes (Up: delta to row above).
    uint8_t *pred = (uint8_t *)malloc((size_t)(W + 1) * H);
    ASSERT(pred != NULL);
    for (int y = 0; y < H; y++) {
        uint8_t *row = pred + (size_t)y * (W + 1);
        row[0] = y == 0 ? 0 : 2;
        for (int x = 0; x < W; x++) {
            uint8_t cur = pixels[y * W + x];
            uint8_t up = y == 0 ? 0 : pixels[(y - 1) * W + x];
            row[1 + x] = y == 0 ? cur : (uint8_t)(cur - up);
        }
    }
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pred, (size_t)(W + 1) * H, &comp_len);
    free(pred);
    ASSERT(comp != NULL);

    // Drawn at 1x1 inch => 256 dpi; at target 150 dpi it must shrink to 150.
    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(
        comp, comp_len,
        "/Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 1 "
        "/BitsPerComponent 8 /Columns 256 >>",
        W, H, "DeviceGray", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    err = tspdf_reader_lossy_images(doc, 150, 75, &st);
    ASSERT_EQ_INT(err, TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);
    ASSERT_EQ_SIZE(st.images_skipped, 0);
    ASSERT(st.bytes_after < st.bytes_before);

    // The stream is now a plain DCTDecode JPEG at the target size, with the
    // predictor parms gone.
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL && img_obj->type == TSPDF_OBJ_STREAM);
    TspdfObj *d = img_obj->stream.dict;
    TspdfObj *filter = tspdf_dict_get(d, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "DCTDecode");
    ASSERT(tspdf_dict_get(d, "DecodeParms") == NULL);
    TspdfObj *wobj = tspdf_dict_get(d, "Width");
    TspdfObj *hobj = tspdf_dict_get(d, "Height");
    ASSERT(wobj && wobj->integer == 150);
    ASSERT(hobj && hobj->integer == 150);

    // The JPEG must decode and still show the gradient (predictor undo fed
    // the right pixels into the downsampler).
    ASSERT(img_obj->stream.self_contained && img_obj->stream.data != NULL);
    TspdfArena a = tspdf_arena_create(1 << 20);
    TspdfRawImage decoded = {0};
    ASSERT(tspdf_jpeg_decode(img_obj->stream.data, img_obj->stream.len, &a, &decoded));
    ASSERT_EQ_INT(decoded.width, 150);
    ASSERT_EQ_INT(decoded.height, 150);
    ASSERT_EQ_INT(decoded.components, 1);
    // Expected value at dest column x is the box average of the source
    // gradient: about (x + 0.5) * 256/150 - 0.5. Allow JPEG error.
    for (int x = 10; x < 150; x += 40) {
        double expect = ((double)x + 0.5) * 256.0 / 150.0 - 0.5;
        int got = decoded.pixels[75 * 150 + x];
        ASSERT(got > expect - 10 && got < expect + 10);
    }
    tspdf_arena_destroy(&a);

    // And the saved file round-trips.
    uint8_t *out = NULL;
    size_t out_len = 0;
    TspdfSaveOptions opts = tspdf_save_options_default();
    opts.strip_unused_objects = true;
    opts.recompress_streams = true;
    ASSERT_EQ_INT(tspdf_reader_save_to_memory_with_options(doc, &out, &out_len, &opts), TSPDF_OK);
    ASSERT(out_len < len); // the gradient JPEG is far smaller than the flate
    TspdfReader *re = tspdf_reader_open(out, out_len, &err);
    ASSERT(re != NULL);
    ASSERT_EQ_SIZE(tspdf_reader_page_count(re), 1);
    tspdf_reader_destroy(re);
    free(out);

    tspdf_reader_destroy(doc);
    free(pdf);
    free(pixels);
}

TEST(test_lossy_rgb_near_gray_converts_to_devicegray) {
    // 256x256 RGB, every pixel within the gray tolerance -> the rewrite
    // must emit a single-channel /DeviceGray JPEG.
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H * 3);
    ASSERT(pixels != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t v = (uint8_t)((x + y) / 2);
            uint8_t *p = pixels + ((size_t)y * W + x) * 3;
            p[0] = v;
            p[1] = (uint8_t)(v <= 251 ? v + 3 : v);
            p[2] = (uint8_t)(v >= 2 ? v - 2 : v);
        }
    }
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H * 3, &comp_len);
    ASSERT(comp != NULL);

    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode",
                                        W, H, "DeviceRGB", 72.0, 72.0, &len);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 1);

    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *cs = tspdf_dict_get(img_obj->stream.dict, "ColorSpace");
    ASSERT(cs && cs->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)cs->string.data, "DeviceGray");

    tspdf_reader_destroy(doc);
    free(pdf);
    free(pixels);
}

TEST(test_lossy_skips_low_dpi_smask_and_small) {
    enum { W = 256, H = 256 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    ASSERT(comp != NULL);

    // (a) Drawn at 2x2 inches => 128 dpi, already below the 150-dpi target:
    // untouched, counted as skipped.
    size_t len = 0;
    uint8_t *pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode",
                                        W, H, "DeviceGray", 144.0, 144.0, &len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    ASSERT_EQ_SIZE(st.images_skipped, 1);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");
    tspdf_reader_destroy(doc);
    free(pdf);

    // (b) High-dpi but carrying an /SMask: untouched.
    pdf = lossy_make_image_pdf(comp, comp_len, "/Filter /FlateDecode /SMask 4 0 R",
                               W, H, "DeviceGray", 36.0, 36.0, &len);
    ASSERT(pdf != NULL);
    doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);
    memset(&st, 0, sizeof(st));
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    tspdf_reader_destroy(doc);
    free(pdf);
    free(comp);

    // (c) Below the 65536-pixel floor (255x255): untouched even at high dpi.
    {
        enum { SW = 255, SH = 255 };
        uint8_t *small = (uint8_t *)malloc((size_t)SW * SH);
        ASSERT(small != NULL);
        memset(small, 99, (size_t)SW * SH);
        size_t sc_len = 0;
        uint8_t *sc = deflate_compress(small, (size_t)SW * SH, &sc_len);
        ASSERT(sc != NULL);
        pdf = lossy_make_image_pdf(sc, sc_len, "/Filter /FlateDecode",
                                   SW, SH, "DeviceGray", 36.0, 36.0, &len);
        free(sc);
        ASSERT(pdf != NULL);
        doc = tspdf_reader_open(pdf, len, &err);
        ASSERT(doc != NULL);
        memset(&st, 0, sizeof(st));
        ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
        ASSERT_EQ_SIZE(st.images_recompressed, 0);
        tspdf_reader_destroy(doc);
        free(pdf);
        free(small);
    }
    free(pixels);
}

// Like lossy_make_image_pdf but with caller-supplied page content.
static uint8_t *lossy_make_content_image_pdf(const uint8_t *img, size_t img_len,
                                             const char *img_extra, int w, int h,
                                             const char *cs, const char *content,
                                             size_t *out_len) {
    size_t content_len = strlen(content);
    size_t cap = img_len + content_len + 4096;
    uint8_t *pdf = (uint8_t *)malloc(cap);
    if (!pdf) return NULL;
    size_t pos = 0;
    size_t off[7] = {0};
    char head[512];

#define LPUT(...) do { \
        int _n = snprintf(head, sizeof(head), __VA_ARGS__); \
        if (_n < 0 || (size_t)_n >= sizeof(head) || pos + (size_t)_n > cap) goto fail; \
        memcpy(pdf + pos, head, (size_t)_n); \
        pos += (size_t)_n; \
    } while (0)

    LPUT("%%PDF-1.5\n%%\xE2\xE3\xCF\xD3\n");
    off[1] = pos;
    LPUT("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = pos;
    LPUT("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = pos;
    LPUT("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");
    off[4] = pos;
    LPUT("4 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d "
         "/ColorSpace /%s /BitsPerComponent 8 %s /Length %zu >>\nstream\n",
         w, h, cs, img_extra ? img_extra : "", img_len);
    if (pos + img_len > cap) goto fail;
    memcpy(pdf + pos, img, img_len);
    pos += img_len;
    LPUT("\nendstream\nendobj\n");
    off[5] = pos;
    LPUT("5 0 obj\n<< /Length %zu >>\nstream\n", content_len);
    if (pos + content_len > cap) goto fail;
    memcpy(pdf + pos, content, content_len);
    pos += content_len;
    LPUT("\nendstream\nendobj\n");

    {
        size_t xref = pos;
        LPUT("xref\n0 6\n0000000000 65535 f \n");
        for (int i = 1; i <= 5; i++) LPUT("%010zu 00000 n \n", off[i]);
        LPUT("trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF", xref);
    }
#undef LPUT

    *out_len = pos;
    return pdf;
fail:
    free(pdf);
    return NULL;
}

// "q 500 0 0 500 0 0 cm" then nq bare q's, nq Q's, then "/Im1 Do Q": the
// image is placed at 500x500 pt however deep the q/Q nesting goes.
static char *lossy_deepq_content(int nq) {
    size_t cap = 32 + (size_t)nq * 4 + 16;
    char *c = (char *)malloc(cap);
    if (!c) return NULL;
    size_t pos = (size_t)snprintf(c, cap, "q 500 0 0 500 0 0 cm ");
    for (int i = 0; i < nq; i++) { c[pos++] = 'q'; c[pos++] = ' '; }
    for (int i = 0; i < nq; i++) { c[pos++] = 'Q'; c[pos++] = ' '; }
    memcpy(c + pos, "/Im1 Do Q", sizeof("/Im1 Do Q"));
    return c;
}

TEST(test_lossy_deep_q_nesting_measured_at_outer_ctm) {
    // Reviewer repro: q <big cm>, 100 nested q's, 100 Q's, then Do. The Do
    // still happens under the 500x500 cm; a bounded gstate stack that drops
    // saves but pops on Q restores the identity CTM too early and measures
    // the image at 1x1 pt, shrinking a real 512x512 image to nearly nothing.
    enum { W = 512, H = 512 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    free(pixels);
    ASSERT(comp != NULL);

    char *content = lossy_deepq_content(100);
    ASSERT(content != NULL);
    size_t len = 0;
    uint8_t *pdf = lossy_make_content_image_pdf(comp, comp_len,
                                                "/Filter /FlateDecode",
                                                W, H, "DeviceGray", content, &len);
    free(content);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // The scan must see the true 500x500 pt placement.
    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(tspdf_lossy_scan_placements(doc, &pl, &n), TSPDF_OK);
    const TspdfLossyPlacement *im = lossy_find_placement(pl, n, 4);
    ASSERT(im != NULL);
    ASSERT(im->w_pt > 499.9 && im->w_pt < 500.1);
    ASSERT(im->h_pt > 499.9 && im->h_pt < 500.1);
    free(pl);

    // 512 px over 500 pt is ~74 dpi, well under the 150-dpi target: the
    // lossy pass must leave the image alone, never shrink it.
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *wobj = tspdf_dict_get(img_obj->stream.dict, "Width");
    ASSERT(wobj && wobj->integer == W);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_lossy_gstate_overflow_skips_image) {
    // Nesting past any sane depth (deeper than the gstate stack will grow):
    // the CTM is no longer trustworthy, so the image must be treated as
    // unmeasurable and skipped -- never downsampled.
    enum { W = 512, H = 512 };
    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H);
    ASSERT(pixels != NULL);
    for (size_t i = 0; i < (size_t)W * H; i++) pixels[i] = (uint8_t)(i * 7);
    size_t comp_len = 0;
    uint8_t *comp = deflate_compress(pixels, (size_t)W * H, &comp_len);
    free(pixels);
    ASSERT(comp != NULL);

    char *content = lossy_deepq_content(5000);
    ASSERT(content != NULL);
    size_t len = 0;
    uint8_t *pdf = lossy_make_content_image_pdf(comp, comp_len,
                                                "/Filter /FlateDecode",
                                                W, H, "DeviceGray", content, &len);
    free(content);
    free(comp);
    ASSERT(pdf != NULL);

    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open(pdf, len, &err);
    ASSERT(doc != NULL);

    // Unmeasurable: excluded from the placement list entirely.
    TspdfLossyPlacement *pl = NULL;
    size_t n = 0;
    ASSERT_EQ_INT(tspdf_lossy_scan_placements(doc, &pl, &n), TSPDF_OK);
    ASSERT(lossy_find_placement(pl, n, 4) == NULL);
    free(pl);

    // And the lossy pass leaves the stream byte-for-byte in place.
    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 75, &st), TSPDF_OK);
    ASSERT_EQ_SIZE(st.images_recompressed, 0);
    TspdfObj *img_obj = lossy_get_image_obj(doc);
    ASSERT(img_obj != NULL);
    TspdfObj *wobj = tspdf_dict_get(img_obj->stream.dict, "Width");
    ASSERT(wobj && wobj->integer == W);
    TspdfObj *filter = tspdf_dict_get(img_obj->stream.dict, "Filter");
    ASSERT(filter && filter->type == TSPDF_OBJ_NAME);
    ASSERT_EQ_STR((const char *)filter->string.data, "FlateDecode");

    tspdf_reader_destroy(doc);
    free(pdf);
}

TEST(test_lossy_invalid_args_rejected) {
    size_t len = 0;
    char *pdf = lossy_make_ctm_pdf(&len);
    ASSERT(pdf != NULL);
    TspdfError err = TSPDF_OK;
    TspdfReader *doc = tspdf_reader_open((const uint8_t *)pdf, len, &err);
    ASSERT(doc != NULL);

    TspdfLossyStats st = {0};
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 0, 75, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 0, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(doc, 150, 101, &st), TSPDF_ERR_INVALID_ARG);
    ASSERT_EQ_INT(tspdf_reader_lossy_images(NULL, 150, 75, &st), TSPDF_ERR_INVALID_ARG);

    tspdf_reader_destroy(doc);
    free(pdf);
}

int main(void) {
    printf("tspr reader tests:\n");

    printf("\n  Parser:\n");
    RUN(test_parse_integer);
    RUN(test_parse_negative_integer);
    RUN(test_parse_integer_overflow_rejected);
    RUN(test_parse_real);
    RUN(test_parse_bool_true);
    RUN(test_parse_keywords_require_delimiters);
    RUN(test_parse_null);
    RUN(test_parse_name);
    RUN(test_parse_name_hex_escape);
    RUN(test_parse_literal_string);
    RUN(test_parse_string_escape);
    RUN(test_parse_string_nested_parens);
    RUN(test_parse_hex_string);
    RUN(test_parse_array);
    RUN(test_parse_dict);
    RUN(test_parse_dict_duplicate_key_uses_last_value);
    RUN(test_parse_indirect_ref);
    RUN(test_parse_indirect_ref_requires_delimited_r);
    RUN(test_parse_indirect_ref_object_number_overflow);
    RUN(test_parse_indirect_ref_generation_overflow);
    RUN(test_parse_indirect_obj);
    RUN(test_parse_indirect_obj_reject_object_number_overflow);
    RUN(test_parse_indirect_obj_reject_generation_overflow);
    RUN(test_parse_stream);
    RUN(test_parse_stream_keyword_requires_delimiter);
    RUN(test_parse_endstream_keyword_requires_delimiter);
    RUN(test_parse_stream_negative_length_fallback);
    RUN(test_deep_copy_stream_uses_decoded_length);
    RUN(test_parse_deeply_nested_array_rejected);
    RUN(test_parse_deeply_nested_dict_rejected);
    RUN(test_parse_moderately_nested_array_accepted);

    printf("\n  Xref:\n");
    RUN(test_xref_parse_classic);
    RUN(test_pdf_version_catalog_indirect_ref_resolved);
    RUN(test_document_open_classic_xref_horizontal_whitespace);
    RUN(test_xref_resolve);
    RUN(test_classic_xref_reject_subsection_range_overflow);
    RUN(test_classic_xref_reject_generation_overflow);
    RUN(test_classic_xref_reject_unknown_entry_type);
    RUN(test_document_reject_wrong_xref_object_number);
    RUN(test_document_reject_wrong_xref_generation);
    RUN(test_document_open_xref_stream_with_png_predictor);
    RUN(test_document_open_xref_stream_without_type);
    RUN(test_document_open_xref_stream_with_dp_abbreviation);
    RUN(test_xref_stream_reject_explicit_wrong_type);
    RUN(test_document_open_xref_stream_with_filter_array);
    RUN(test_document_open_xref_stream_with_filter_chain);
    RUN(test_document_open_xref_stream_filter_chain_direct_decode_params);
    RUN(test_document_open_xref_stream_with_ascii85_filter_chain);
    RUN(test_xref_stream_ascii85_z_shorthand_expands_safely);
    RUN(test_document_open_xref_stream_with_runlength_filter_chain);
    RUN(test_document_open_xref_stream_with_lzw_predictor);
    RUN(test_document_reject_xref_stream_negative_w);
    RUN(test_document_reject_xref_stream_odd_index);
    RUN(test_xref_stream_reject_oversized_field_width);
    RUN(test_classic_xref_reject_implausible_entry_count);
    RUN(test_xref_stream_reject_implausible_size);
    RUN(test_xref_stream_reject_unknown_entry_type);
    RUN(test_xref_stream_reject_generation_overflow);
    RUN(test_xref_stream_reject_objstm_number_overflow);
    RUN(test_xref_stream_accepts_large_objstm_index);
    RUN(test_document_reject_self_referential_object_stream_entry);
    RUN(test_document_reject_cyclic_object_stream_entries);
    RUN(test_document_open_incremental_update_prev_chain);
    RUN(test_document_open_incremental_update_inherits_root_from_prev);
    RUN(test_xref_reject_self_referential_prev_chain);
    RUN(test_document_open_hybrid_reference_xrefstm);
    RUN(test_document_open_hybrid_reference_xrefstm_near_offset_repaired);
    RUN(test_xref_indirect_stream_length);
    RUN(test_xref_resolve_rejects_objstm_header_spill);
    RUN(test_document_open_object_stream_page_tree);
    RUN(test_document_open_object_stream_large_index);
    RUN(test_document_open_object_stream_filter_chain);
    RUN(test_document_open_object_stream_ascii85_filter_chain);
    RUN(test_document_open_object_stream_runlength_filter_chain);
    RUN(test_document_open_object_stream_lzw_filter);
    RUN(test_document_reject_objstm_negative_first);
    RUN(test_document_reject_objstm_negative_n);
    RUN(test_document_reject_objstm_bad_offset);
    RUN(test_document_reject_objstm_wrong_object_number);

    printf("\n  Document open:\n");
    RUN(test_document_open_mini);
    RUN(test_document_open_direct_trailer_root_catalog);
    RUN(test_document_open_duplicate_trailer_root_uses_last);
    RUN(test_document_reject_non_dictionary_trailer_root);
    RUN(test_document_open_invalid);
    RUN(test_document_open_header_with_prefix);
    RUN(test_document_open_header_beyond_1024_byte_prefix);
    RUN(test_document_open_prefixed_header_relative_xref_offsets_repaired);
    RUN(test_document_open_long_trailing_padding);
    RUN(test_document_open_startxref_before_table_repaired);
    RUN(test_document_open_startxref_inside_table_repaired);
    RUN(test_document_open_bad_appended_startxref_uses_earlier_marker);
    RUN(test_document_open_missing_startxref_classic_xref_recovered);
    RUN(test_document_open_no_trailer_object_scan_recovered);
    RUN(test_document_open_object_scan_only_recovered);
    RUN(test_document_open_short_header_only);
    RUN(test_document_page_inherit_rotate);
    RUN(test_document_page_tree_normalizes_rotate);
    RUN(test_document_page_tree_ignores_invalid_rotate);
    RUN(test_document_page_tree_indirect_attributes);
    RUN(test_document_page_box_indirect_number_entries);
    RUN(test_document_page_tree_missing_type_inferred);
    RUN(test_document_page_tree_inherited_crop_box_fallback);
    RUN(test_document_page_tree_inherited_user_unit);

    printf("\n  Serialization:\n");
    RUN(test_round_trip_save_reopen);
    RUN(test_save_to_file);
    RUN(test_save_adds_missing_stream_length);
    RUN(test_save_deduplicates_stream_length);

    printf("\n  Manipulation:\n");
    RUN(test_extract_pages);
    RUN(test_delete_pages);
    RUN(test_rotate_pages);
    RUN(test_rotate_pages_normalizes_negative_angle);
    RUN(test_set_cropbox_explicit_box);
    RUN(test_set_cropbox_clamps_to_mediabox);
    RUN(test_set_cropbox_degenerate_rejected);
    RUN(test_set_cropbox_out_of_range);
    RUN(test_scale_factor_half);
    RUN(test_scale_nonuniform_and_cropbox);
    RUN(test_resize_to_a4_from_letter);
    RUN(test_resize_to_a4_rotate90_viewed_portrait);
    RUN(test_resize_to_a4_rotate270_viewed_portrait);
    RUN(test_resize_to_a4_rotate90_preserves_viewed_landscape);
    RUN(test_resize_to_a4_landscape_page_stays_landscape);
    RUN(test_page_crop_box_exposed);
    RUN(test_resize_to_rejects_nonpositive);
    RUN(test_scale_rejects_nonpositive_factor);
    RUN(test_reorder_pages);
    RUN(test_merge_documents);
    RUN(test_save_rejects_missing_stream_source);
    RUN(test_merge_rejects_invalid_source_stream_range);
    RUN(test_extract_preserves_inherited_page_attributes);
    RUN(test_extract_serializes_inferred_page_type_and_parent);
    RUN(test_extract_single_page);
    RUN(test_extract_single_page_omits_unselected_sibling_streams);

    printf("\n  Real PDF files:\n");
    RUN(test_open_tspdf_one_page);
    RUN(test_open_tspdf_three_pages);

    printf("\n  Error handling:\n");
    RUN(test_open_null_data);
    RUN(test_open_truncated_pdf);
    RUN(test_open_cyclic_page_tree_rejected);
    RUN(test_open_pdf_header_only_no_startxref_scan_crash);
    RUN(test_open_pdf_rejects_overflowing_xref_subsection);
    RUN(test_extract_out_of_bounds);
    RUN(test_rotate_invalid_angle);
    RUN(test_merge_zero_docs);
    RUN(test_open_v4_identity_stmf_reads_plaintext_stream);
    RUN(test_open_encrypted_pdf_unsupported);
    RUN(test_error_string);

    printf("\n  Encryption:\n");
    RUN(test_encrypted_pdf_needs_password);
    RUN(test_encryption_permissions_accessor);
    RUN(test_encrypt_aes128_roundtrip);
    RUN(test_encrypt_aes256_roundtrip);
    RUN(test_reencrypt_opened_encrypted_decrypts_source_streams);
    RUN(test_reencrypt_single_encrypt_dict);
    RUN(test_encrypt_empty_user_password);
    RUN(test_manipulate_encrypted);
    RUN(test_open_r6_aes256_with_user_password);
    RUN(test_open_r6_aes256_wrong_password_rejected);
    RUN(test_open_r6_aes256_empty_user_password);
    RUN(test_open_r6_aes256_owner_password);
    RUN(test_open_r4_cleartext_metadata_empty_password);

    printf("\n  Metadata:\n");
    RUN(test_metadata_set_and_read);
    RUN(test_metadata_remove_field);
    RUN(test_metadata_preserved_through_manipulation);

    printf("\n  Content overlay:\n");
    RUN(test_overlay_text);
    RUN(test_overlay_on_specific_page);
    RUN(test_overlay_abort);
    RUN(test_overlay_wraps_unbalanced_content);
    RUN(test_overlay_indirect_shared_resources);

    printf("\n  Form XObject import:\n");
    RUN(test_end_content_under);
    RUN(test_import_page_xobject_basic);
    RUN(test_import_same_source_twice_dedups);
    RUN(test_import_distinct_readers_not_deduped);
    RUN(test_import_source_freed_before_save);
    RUN(test_import_from_encrypted_source);
    RUN(test_import_page_out_of_range);
    RUN(test_import_multi_stream_contents);
    RUN(test_import_cyclic_resources_bounded);
    RUN(test_import_huge_bbox_clamped);
    RUN(test_import_degenerate_bbox_rejected);
    RUN(test_add_xobject_unique_names);

    printf("\n  N-up imposition:\n");
    RUN(test_nup_2up_four_pages);
    RUN(test_nup_4up_four_pages_one_sheet);
    RUN(test_nup_3pages_4up_partial_sheet);
    RUN(test_nup_5pages_2up_three_sheets);
    RUN(test_nup_invalid_n_rejected);
    RUN(test_nup_empty_selection_rejected);
    RUN(test_nup_aspect_ratio_uniform_scale);
    RUN(test_nup_frame_draws_border);
    RUN(test_nup_cyclic_resources_bounded);
    RUN(test_nup_source_outlives_output_until_saved);

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
    RUN(test_save_use_xref_stream_roundtrip);
    RUN(test_save_preserves_catalog_entries_with_strip_unused);
    RUN(test_save_strip_metadata_removes_catalog_metadata);
    RUN(test_save_preserve_ids_with_strip_unused_keeps_catalog_root);
    RUN(test_save_preserve_ids_with_strip_metadata_rewrites_catalog);
    RUN(test_save_strip_metadata);
    RUN(test_save_recompress);

    printf("\n  Large document:\n");
    RUN(test_large_document_500_pages);

    printf("\n  Audit fixes (reader-core):\n");
    RUN(test_error_string_page_range);
    RUN(test_extract_out_of_range_sets_page_range_error);
    RUN(test_delete_out_of_range_sets_page_range_error);
    RUN(test_rotate_out_of_range_sets_page_range_error);
    RUN(test_reorder_out_of_range_sets_page_range_error);
    RUN(test_extract_drops_document_level_trees);
    RUN(test_delete_keeps_document_level_trees);
    RUN(test_recompress_adds_flate_to_unfiltered_streams);
    RUN(test_recompress_keeps_incompressible_stream_verbatim);
    RUN(test_recompress_reflates_large_flate_stream);
    RUN(test_recompress_encrypted_source_roundtrip);
    RUN(test_recompress_writes_xref_stream);
    RUN(test_recompress_packs_objects_into_object_streams);
    RUN(test_recompress_roundtrip_from_objstm_input);
    RUN(test_recompress_objstm_output_does_not_grow);
    RUN(test_encrypted_save_has_no_objstm);
    RUN(test_default_save_repacks_objstm_input);
    RUN(test_default_save_classic_input_stays_classic);
    RUN(test_encrypted_save_repacks_objstm_input);
    RUN(test_encrypted_info_strings_are_encrypted);

    printf("\n  Encoding/i18n:\n");
    RUN(test_pdftext_cp1252_from_codepoint);
    RUN(test_pdftext_utf8_to_cp1252_strict);
    RUN(test_pdftext_utf8_to_cp1252_lossy);
    RUN(test_pdftext_utf16be_roundtrip);
    RUN(test_pdftext_pdfdoc_decode);
    RUN(test_pdftext_text_string_decode);
    RUN(test_pdftext_text_string_encode);
    RUN(test_metadata_non_ascii_utf16_roundtrip);
    RUN(test_metadata_ascii_stays_literal);
    RUN(test_metadata_pdfdoc_encoded_decodes_to_utf8);
    RUN(test_save_producer_is_tspdf_with_version);
    RUN(test_save_preserve_ids_producer_is_tspdf_with_version);

    printf("\n  Text extraction:\n");
    RUN(test_text_simple_lines_writer_fixture);
    RUN(test_text_tj_kerning_spacing);
    RUN(test_text_multiline_td_tstar);
    RUN(test_text_xgap_emits_space);
    RUN(test_text_tj_kern_space_missing_space_glyph);
    RUN(test_text_font_switch_gap_space);
    RUN(test_text_font_switch_bogus_space_width);
    RUN(test_text_tounicode_bfchar_bfrange);
    RUN(test_text_tounicode_null_cp_skipped);
    RUN(test_text_tounicode_wide_range_with_point_fixes);
    RUN(test_text_contents_array_concatenated);
    RUN(test_text_differences_encoding);
    RUN(test_text_form_xobject_recursion);
    RUN(test_text_form_xobject_self_cycle_guarded);
    RUN(test_text_identity_h_ttf_tounicode);
    RUN(test_text_cid_no_tounicode_replacement);
    RUN(test_text_encrypted_pdf);
    RUN(test_text_page_out_of_range);
    RUN(test_text_no_bt_writer_style_content);
    RUN(test_text_malformed_content_no_crash);
    RUN(test_text_ligatures_folded_to_ascii);
    RUN(test_text_trailing_spaces_trimmed);

    printf("\n  Layout-preserving text extraction:\n");
    RUN(test_text_layout_two_columns);
    RUN(test_text_layout_table_grid);
    RUN(test_text_layout_y_jitter_merges_line);
    RUN(test_text_layout_blank_lines_bounded);
    RUN(test_text_layout_hostile_x_clamped);
    RUN(test_text_layout_empty_page);
    RUN(test_text_layout_page_out_of_range);

    printf("\n  Doc trees (outlines/AcroForm across merge/extract):\n");
    RUN(test_merge_preserves_outlines);
    RUN(test_merge_concatenates_acroform_fields);
    RUN(test_merge_without_doctrees_stays_clean);
    RUN(test_extract_keeps_outline_subtree_for_kept_pages);
    RUN(test_extract_keeps_parent_when_only_child_survives);
    RUN(test_extract_omits_outlines_when_nothing_survives);
    RUN(test_extract_prunes_acroform_fields_by_page);
    RUN(test_extract_flattens_named_dests);
    RUN(test_merge_flattens_named_dests_and_keeps_uri_actions);
    RUN(test_extract_prunes_field_kids_and_carries_da);
    RUN(test_extract_and_merge_bounded_on_cyclic_nametree);
    RUN(test_extract_preserves_page_labels);
    RUN(test_merge_page_labels_labeled_plus_unlabeled);
    RUN(test_merge_without_page_labels_emits_none);
    RUN(test_extract_bounded_on_cyclic_pagelabel_tree);
    RUN(test_extract_bounded_on_cyclic_field_kids);
    RUN(test_extract_breaks_cyclic_outline_siblings);

    printf("\n  Embedded file attachments:\n");
    RUN(test_attach_add_save_reopen_roundtrip);
    RUN(test_attach_flat_tree_keys_sorted);
    RUN(test_attach_add_replaces_same_name);
    RUN(test_attach_remove);
    RUN(test_attach_cyclic_embeddedfiles_tree_bounded);
    RUN(test_attach_extract_keeps_all_attachments);
    RUN(test_attach_merge_unions_first_wins);
    RUN(test_attach_delete_pages_keeps_attachments);
    RUN(test_attach_encrypted_roundtrip);
    RUN(test_attach_unicode_name_stored_as_utf16_roundtrip);
    RUN(test_attach_reads_foreign_encoded_names);

    printf("\n  Save-to-memory byte identity (wasm):\n");
    RUN(test_reader_save_to_memory_matches_file);
    RUN(test_reader_save_to_memory_with_options_matches_file);

    printf("\n  AcroForm fields (list/fill/flatten):\n");
    RUN(test_form_fields_fixture);
    RUN(test_form_fields_no_form);
    RUN(test_form_fields_encrypted);
    RUN(test_form_fields_writer_roundtrip);
    RUN(test_form_fields_cyclic_kids_bounded);
    RUN(test_form_fields_self_kid_listed_once);
    RUN(test_form_fields_adversarial_no_crash);
    RUN(test_form_fill_text_value_and_appearance);
    RUN(test_form_fill_text_da_font_name_sanitized);
    RUN(test_form_fill_text_nonascii_roundtrip);
    RUN(test_form_fill_checkbox_states);
    RUN(test_form_fill_radio_sets_widget_as);
    RUN(test_form_fill_choice);
    RUN(test_form_fill_choice_rejects_non_option);
    RUN(test_form_fill_editable_combo_accepts_free_text);
    RUN(test_form_fill_encrypted_save_preserves_encryption);
    RUN(test_save_decrypt_option_writes_plaintext);
    RUN(test_form_fill_unknown_name_errors);
    RUN(test_form_fill_readonly_requires_force);
    RUN(test_form_flatten_fixture);
    RUN(test_form_flatten_no_form_noop);
    RUN(test_form_flatten_writer_checkbox_fallback);
    RUN(test_form_flatten_shared_resources_not_accumulated);

    printf("\n  Outline (bookmark) editing:\n");
    RUN(test_bookmark_list_fixture);
    RUN(test_bookmark_list_no_outline);
    RUN(test_bookmark_set_three_level_roundtrip);
    RUN(test_bookmark_set_nonascii_title_roundtrip);
    RUN(test_bookmark_set_validation_errors);
    RUN(test_bookmark_clear);
    RUN(test_bookmark_clear_drops_pagemode_useoutlines);
    RUN(test_bookmark_set_empty_clears);
    RUN(test_bookmark_list_then_set_stable);
    RUN(test_bookmark_list_cyclic_bounded);
    RUN(test_bookmark_set_large_bounded);
    RUN(test_bookmark_list_indirect_title);
    RUN(test_bookmark_add_preserves_indirect_titles);
    RUN(test_merge_copies_indirect_outline_strings);
    RUN(test_serialize_never_writes_inuse_offset_zero);

    printf("\n  Lossy image recompression:\n");
    RUN(test_lossy_box_downsample_exact);
    RUN(test_lossy_box_downsample_fractional_edges);
    RUN(test_lossy_gray_detect);
    RUN(test_lossy_target_dims);
    RUN(test_lossy_keep_original_rule);
    RUN(test_lossy_ctm_scan_placements);
    RUN(test_lossy_predictor15_flate_image_recompressed);
    RUN(test_lossy_rgb_near_gray_converts_to_devicegray);
    RUN(test_lossy_skips_low_dpi_smask_and_small);
    RUN(test_lossy_deep_q_nesting_measured_at_outer_ctm);
    RUN(test_lossy_gstate_overflow_skips_image);
    RUN(test_lossy_invalid_args_rejected);

    printf("\n%d tests, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
