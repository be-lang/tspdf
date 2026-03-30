#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool _test_failed;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    tests_run++; \
    _test_failed = false; \
    printf("  %-50s", #name); \
    name(); \
    if (!_test_failed) { tests_passed++; printf("PASS\n"); } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        _test_failed = true; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; _test_failed = true; return; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
        tests_failed++; _test_failed = true; return; \
    } \
} while(0)

#define ASSERT_EQ_SIZE(a, b) do { \
    size_t _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %s == %zu, expected %zu\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; _test_failed = true; return; \
    } \
} while(0)

#endif
