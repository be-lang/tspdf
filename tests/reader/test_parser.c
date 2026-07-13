#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

#include "../test_framework.h"
#include "../../src/reader/tspr.h"
#include "../../src/reader/tspr_overlay.h"
#include "../../src/reader/tspr_internal.h"
#include "../../src/pdf/pdf_stream.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/util/arena.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/font/ttf_parser.h"
#include "../../src/font/font_fallback.h"
#include "../../src/crypto/md5.h"
#include "../../include/tspdf/version.h"

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

void run_parser_tests(void) {
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
}
