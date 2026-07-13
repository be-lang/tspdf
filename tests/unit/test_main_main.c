#include <stdio.h>
#include <stdbool.h>

#include "../test_framework.h"

extern void run_util_tests(void);
extern void run_codecs_tests(void);
extern void run_writer_tests(void);
extern void run_qr_tests(void);

int main(void) {
    printf("Running tests...\n\n");
    run_util_tests();
    run_codecs_tests();
    run_writer_tests();
    run_qr_tests();

    printf("\n%d tests run, %d passed, %d failed, %d skipped\n",
           tests_run, tests_passed, tests_failed, tests_skipped);
    if (tests_skipped > 0) {
        printf("note: %d test(s) skipped (missing optional system font) — "
               "TTF subsetting / Unicode paths were NOT exercised\n",
               tests_skipped);
    }
    return tests_failed > 0 ? 1 : 0;
}
