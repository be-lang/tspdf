#include "../test_framework.h"

extern void run_parser_tests(void);
extern void run_xref_tests(void);
extern void run_serialize_tests(void);
extern void run_crypt_tests(void);
extern void run_docops_tests(void);
extern void run_import_tests(void);
extern void run_text_tests(void);
extern void run_form_tests(void);
extern void run_bookmark_tests(void);
extern void run_lossy_tests(void);

int main(void) {
    printf("tspr reader tests:\n");
    run_parser_tests();
    run_xref_tests();
    run_serialize_tests();
    run_crypt_tests();
    run_docops_tests();
    run_import_tests();
    run_text_tests();
    run_form_tests();
    run_bookmark_tests();
    run_lossy_tests();
    printf("\n%d tests, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
