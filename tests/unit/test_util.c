#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "../test_framework.h"

#include "../../src/util/buffer.h"
#include "../../src/util/arena.h"
#include "../../src/compress/deflate.h"
#include "../../src/image/png_decoder.h"
#include "../../src/image/jpeg_codec.h"
#include "../../src/image/ccitt_codec.h"
#include "../../src/pdf/pdf_base14.h"
#include "../../src/layout/layout.h"
#include "../../src/pdf/tspdf_writer.h"
#include "../../src/tspdf_error.h"
#include "../../src/qr/qr_encode.h"
#include "../../src/util/pdfdate.h"

// ============================================================
// TspdfBuffer tests
// ============================================================

TEST(test_buffer_create) {
    TspdfBuffer b = tspdf_buffer_create(64);
    ASSERT(b.data != NULL);
    ASSERT_EQ_INT((int)b.len, 0);
    ASSERT(b.capacity >= 64);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append) {
    TspdfBuffer b = tspdf_buffer_create(16);
    const char *msg = "hello";
    tspdf_buffer_append(&b, msg, 5);
    ASSERT_EQ_INT((int)b.len, 5);
    ASSERT(memcmp(b.data, "hello", 5) == 0);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_printf) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_printf(&b, "num=%d", 42);
    ASSERT_EQ_INT((int)b.len, 6);
    ASSERT(memcmp(b.data, "num=42", 6) == 0);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_reset) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_append_str(&b, "test");
    ASSERT(b.len == 4);
    size_t cap = b.capacity;
    tspdf_buffer_reset(&b);
    ASSERT_EQ_INT((int)b.len, 0);
    ASSERT(b.capacity == cap);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, 72.0, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed > 71.9999 && parsed < 72.0001);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double_precision) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, 3.14159, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed > 3.1415 && parsed < 3.1417);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_double_negative) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_double(&b, -0.5, 4);
    tspdf_buffer_append_byte(&b, '\0');
    double parsed = 0;
    sscanf((const char *)b.data, "%lf", &parsed);
    ASSERT(parsed < -0.4999 && parsed > -0.5001);
    tspdf_buffer_destroy(&b);
}

TEST(test_buffer_append_int) {
    TspdfBuffer b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, 42);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "42");
    tspdf_buffer_destroy(&b);

    b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, -7);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "-7");
    tspdf_buffer_destroy(&b);

    b = tspdf_buffer_create(64);
    tspdf_buffer_append_int(&b, 0);
    tspdf_buffer_append_byte(&b, '\0');
    ASSERT_EQ_STR((const char *)b.data, "0");
    tspdf_buffer_destroy(&b);
}

// A growth request that would overflow size_t (len + needed wraps) must be
// refused: set buf->error, leave len/capacity untouched, and never write.
TEST(test_buffer_reject_grow_overflow) {
    TspdfBuffer b = tspdf_buffer_create(16);
    tspdf_buffer_append_str(&b, "abc");
    size_t saved_len = b.len;
    size_t saved_cap = b.capacity;
    // SIZE_MAX bytes can never fit alongside the existing 3; must not wrap.
    tspdf_buffer_append(&b, "x", SIZE_MAX);
    ASSERT(b.error);
    ASSERT(b.len == saved_len);
    ASSERT(b.capacity == saved_cap);
    // The original 3 bytes must remain intact (no shrinking realloc / overrun).
    ASSERT(memcmp(b.data, "abc", 3) == 0);
    tspdf_buffer_destroy(&b);
}

// A normal append must still grow correctly after the overflow-safe rewrite.
TEST(test_buffer_grow_beyond_initial) {
    TspdfBuffer b = tspdf_buffer_create(8);
    char payload[100];
    memset(payload, 'z', sizeof(payload));
    tspdf_buffer_append(&b, payload, sizeof(payload));
    ASSERT(!b.error);
    ASSERT_EQ_INT((int)b.len, (int)sizeof(payload));
    ASSERT(b.capacity >= sizeof(payload));
    ASSERT(memcmp(b.data, payload, sizeof(payload)) == 0);
    tspdf_buffer_destroy(&b);
}

// ============================================================
// TspdfArena tests
// ============================================================

TEST(test_arena_create) {
    TspdfArena a = tspdf_arena_create(4096);
    ASSERT(a.first != NULL);
    ASSERT(tspdf_arena_remaining(&a) > 0);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_alloc_alignment) {
    TspdfArena a = tspdf_arena_create(4096);
    void *p1 = tspdf_arena_alloc(&a, 1);
    void *p2 = tspdf_arena_alloc(&a, 1);
    ASSERT(p1 != NULL);
    ASSERT(p2 != NULL);
    ASSERT(((uintptr_t)p2 % 8) == 0);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_growth) {
    TspdfArena a = tspdf_arena_create(64);
    // With growing arena, allocations beyond initial capacity succeed
    void *p = tspdf_arena_alloc(&a, 128);
    ASSERT(p != NULL);
    // Should have grown: total_allocated > 64
    ASSERT(a.total_allocated > 64);
    tspdf_arena_destroy(&a);
}

TEST(test_arena_reset) {
    TspdfArena a = tspdf_arena_create(256);
    tspdf_arena_alloc(&a, 128);
    size_t rem1 = tspdf_arena_remaining(&a);
    tspdf_arena_reset(&a);
    size_t rem2 = tspdf_arena_remaining(&a);
    ASSERT(rem2 > rem1);
    tspdf_arena_destroy(&a);
}

// A size whose 8-byte alignment round-up would wrap size_t must fail cleanly
// (return NULL) rather than under-allocate a block the caller then overruns.
TEST(test_arena_reject_alloc_overflow) {
    TspdfArena a = tspdf_arena_create(256);
    void *p = tspdf_arena_alloc(&a, SIZE_MAX - 3);  // (size+7) wraps
    ASSERT(p == NULL);
    // Arena must remain usable for legitimate allocations afterward.
    void *q = tspdf_arena_alloc(&a, 32);
    ASSERT(q != NULL);
    tspdf_arena_destroy(&a);
}

void run_util_tests(void) {
    printf("  TspdfBuffer:\n");
    RUN(test_buffer_create);
    RUN(test_buffer_append);
    RUN(test_buffer_printf);
    RUN(test_buffer_reset);
    RUN(test_buffer_append_double);
    RUN(test_buffer_append_double_precision);
    RUN(test_buffer_append_double_negative);
    RUN(test_buffer_append_int);
    RUN(test_buffer_reject_grow_overflow);
    RUN(test_buffer_grow_beyond_initial);

    printf("\n  TspdfArena:\n");
    RUN(test_arena_create);
    RUN(test_arena_alloc_alignment);
    RUN(test_arena_growth);
    RUN(test_arena_reset);
    RUN(test_arena_reject_alloc_overflow);
}
