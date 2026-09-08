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


int main(void)
{
    test_invalid_rate_has_no_period();
    test_exact_rate_period();
    test_non_integral_period_is_rounded();
    puts("AD7606 acquisition timing tests passed");
    return 0;
}
