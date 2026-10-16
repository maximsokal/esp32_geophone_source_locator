#include <assert.h>
#include <stdint.h>

#include "app_tick.h"

static void test_subtick_conversion_is_never_zero_for_positive_waits(void)
{
    for (uint32_t milliseconds = 1U; milliseconds < 10U; ++milliseconds) {
        assert((milliseconds * 100U) / 1000U == 0U);
        assert(app_ms_to_ticks_min_one(milliseconds, 100U) == 1U);
    }
    assert(app_ms_to_ticks_min_one(0U, 100U) == 0U);
    assert(app_ms_to_ticks_min_one(10U, 100U) == 1U);
    assert(app_ms_to_ticks_min_one(20U, 100U) == 2U);
    assert(app_ms_to_ticks_min_one(1U, 1000U) == 1U);
    assert(app_ms_to_ticks_min_one(1U, 0U) == 0U);
}

int main(void)
{
    test_subtick_conversion_is_never_zero_for_positive_waits();
    return 0;
}
