#include <stdio.h>

#include "unity.h"
#include "unity_test_runner.h"


void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    const int failures = UNITY_END();

    printf(
        "TEST_APP_RESULT: %s tests=%d failures=%d\n",
        failures == 0 ? "PASS" : "FAIL",
        unity_get_test_count(),
        failures
    );
}
