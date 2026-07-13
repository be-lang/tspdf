#include "test_framework.h"

int tests_run     = 0;
int tests_passed  = 0;
int tests_failed  = 0;
int tests_skipped = 0;

bool _test_failed;
bool _test_skipped;
const char *_test_skip_reason;
