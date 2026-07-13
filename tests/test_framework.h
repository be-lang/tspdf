#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;
extern int tests_skipped;

extern bool _test_failed;
extern bool _test_skipped;
extern const char *_test_skip_reason;

#define TEST(name) static void name(void)

/* RUN executes a test and classifies the outcome as PASS, FAIL, or SKIP.
 * A skip is a test that could not run for an environmental reason (e.g. an
 * optional system font is absent) rather than a pass: it is counted and
 * printed distinctly so a clean box does not silently report unexecuted
 * coverage as green. RUN references every skip-tracking variable so each
 * translation unit that includes this header keeps them "used" and stays
 * warning-clean under -Wall -Wextra even when it never calls SKIP(). */
#define RUN(name) do { \
    tests_run++; \
    _test_failed = false; \
    _test_skipped = false; \
    _test_skip_reason = ""; \
    printf("  %-50s", #name); \
    name(); \
    if (_test_failed) { \
        /* failure already counted/printed by the assertion that tripped */ \
    } else if (_test_skipped) { \
        tests_skipped++; \
        printf("SKIP (%s)\n", _test_skip_reason); \
    } else { \
        tests_passed++; \
        printf("PASS\n"); \
    } \
} while(0)

/* SKIP marks the current test as skipped and returns from it. Use when a
 * precondition outside the code under test is missing (e.g. a system font),
 * so the path is reported as SKIP rather than masquerading as PASS. */
#define SKIP(reason) do { \
    _test_skipped = true; \
    _test_skip_reason = (reason); \
    return; \
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
