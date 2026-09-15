#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ad7606_acquisition_timing.h"


static void test_invalid_rate_has_no_period(void)
{
    assert(ad7606_acquisition_period_us(0U) == 0U);
}


static void test_exact_rate_period(void)
{
    /* The production configuration is 500 Hz: exactly 2 ms per frame. */
    assert(ad7606_acquisition_period_us(500U) == 2000U);
    assert(ad7606_acquisition_period_us(5000U) == 200U);
}


static void test_non_integral_period_is_rounded(void)
{
    /* 333 Hz is 3003.003... us; nearest integral period is 3003 us. */
    assert(ad7606_acquisition_period_us(333U) == 3003U);
    assert(ad7606_acquisition_period_us(128U) == 7813U);
}


static void test_collapsed_notification_accounting(void)
{
    assert(ad7606_acquisition_collapsed_periods(0U) == 0U);
    assert(ad7606_acquisition_collapsed_periods(1U) == 0U);
    assert(ad7606_acquisition_collapsed_periods(2U) == 1U);
    assert(ad7606_acquisition_collapsed_periods(7U) == 6U);
}


static void test_saturating_counter_addition(void)
{
    assert(ad7606_acquisition_saturating_add_u64(10U, 20U) == 30U);
    assert(
        ad7606_acquisition_saturating_add_u64(UINT64_MAX - 2U, 1U) ==
        UINT64_MAX - 1U
    );
    assert(
        ad7606_acquisition_saturating_add_u64(UINT64_MAX - 2U, 3U) ==
        UINT64_MAX
    );
    assert(
        ad7606_acquisition_saturating_add_u64(UINT64_MAX, 1U) ==
        UINT64_MAX
    );
}


int main(void)
{
    test_invalid_rate_has_no_period();
    test_exact_rate_period();
    test_non_integral_period_is_rounded();
    test_collapsed_notification_accounting();
    test_saturating_counter_addition();
    puts("AD7606 acquisition timing tests passed");
    return 0;
}
