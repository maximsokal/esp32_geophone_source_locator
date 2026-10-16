#include <assert.h>
#include <stdint.h>

#include "nrf_timing.h"

static void test_u32_elapsed_with_a_u64_clock(void)
{
    const uint64_t wrapped_now = (uint64_t)UINT32_MAX + 50U;
    const uint32_t start_before_wrap = UINT32_MAX - 100U;

    assert(nrf_timing_ms_until_u32(wrapped_now, start_before_wrap, 300U) == 150U);
    assert(nrf_timing_ms_until_u32(
               (UINT64_C(5) << 32U) + 150U,
               100U,
               100U) == 50U);
    assert(nrf_timing_ms_until_u32(wrapped_now, start_before_wrap, 150U) == 0U);
}

static void test_phase_locked_64_bit_deadlines(void)
{
    const uint64_t far_due = (UINT64_C(1) << 40U) + 1000U;

    assert(nrf_timing_next_periodic_deadline_ms(1000U, 3500U, 1000U) == 4000U);
    assert(nrf_timing_next_periodic_deadline_ms(1000U, 4000U, 1000U) == 5000U);
    assert(nrf_timing_next_periodic_deadline_ms(5000U, 4000U, 1000U) == 5000U);
    assert(nrf_timing_next_periodic_deadline_ms(
               far_due,
               far_due + 2500U,
               1000U) == far_due + 3000U);
    assert(nrf_timing_next_periodic_deadline_ms(
               UINT64_MAX - 10U,
               UINT64_MAX,
               100U) == UINT64_MAX);
}

int main(void)
{
    test_u32_elapsed_with_a_u64_clock();
    test_phase_locked_64_bit_deadlines();
    return 0;
}
