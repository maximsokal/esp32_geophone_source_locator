#include <assert.h>
#include <stdint.h>

#include "app_tick.h"
#include "freertos/FreeRTOS.h"

static uint32_t delay_until_next_start(
    uint32_t *last_wake_tick,
    uint32_t current_start_tick,
    uint32_t work_ticks,
    uint32_t period_ticks,
    uint32_t *missed_out)
{
    const uint32_t now_tick = current_start_tick + work_ticks;
    *missed_out = app_tick_skip_missed_periods(last_wake_tick, now_tick, period_ticks);
    const uint32_t next_deadline_tick = *last_wake_tick + period_ticks;
    const uint32_t wait_ticks = next_deadline_tick - now_tick;
    *last_wake_tick = next_deadline_tick;
    return wait_ticks < UINT32_C(0x80000000) ? now_tick + wait_ticks : now_tick;
}

static void test_characterization_pdms_uses_configured_100_hz_tick(void)
{
    assert(CONFIG_FREERTOS_HZ == 100U);
    assert(configTICK_RATE_HZ == CONFIG_FREERTOS_HZ);
    for (uint32_t milliseconds = 1U; milliseconds < 10U; ++milliseconds) {
        assert(pdMS_TO_TICKS(milliseconds) == 0U);
        assert(app_ms_to_ticks_min_one(milliseconds, configTICK_RATE_HZ) == 1U);
    }
    assert(pdMS_TO_TICKS(10U) == 1U);
    assert(pdMS_TO_TICKS(20U) == 2U);
}

static void test_mutation_only_deadline_boundaries_and_tick_wrap(void)
{
    assert(app_tick_skip_missed_periods(NULL, 110U, 10U) == 0U);
    uint32_t last_wake_tick = 100U;
    assert(app_tick_skip_missed_periods(&last_wake_tick, 110U, 0U) == 0U);
    assert(app_tick_skip_missed_periods(&last_wake_tick, 110U, UINT32_C(0x80000000)) == 0U);
    assert(last_wake_tick == 100U);

    assert(app_tick_skip_missed_periods(&last_wake_tick, 109U, 10U) == 0U);
    assert(last_wake_tick == 100U);
    assert(app_tick_skip_missed_periods(&last_wake_tick, 110U, 10U) == 0U);
    assert(last_wake_tick == 100U);
    assert(app_tick_skip_missed_periods(&last_wake_tick, 111U, 10U) == 1U);
    assert(last_wake_tick == 110U);

    last_wake_tick = UINT32_MAX - 5U;
    assert(app_tick_skip_missed_periods(&last_wake_tick, 3U, 10U) == 0U);
    assert(app_tick_skip_missed_periods(&last_wake_tick, 4U, 10U) == 0U);
    assert(app_tick_skip_missed_periods(&last_wake_tick, 5U, 10U) == 1U);
    assert(last_wake_tick == 4U);

    last_wake_tick = 120U;
    assert(app_tick_skip_missed_periods(&last_wake_tick, 150U, 10U) == 2U);
    assert(last_wake_tick == 140U);
}

static void assert_periodic_work_contract(uint32_t work_ticks, uint32_t period_ticks)
{
    uint32_t last_wake_tick = 0U;
    uint32_t absolute_start_tick = 0U;
    uint32_t relative_start_tick = 0U;
    uint32_t total_missed = 0U;
    const uint32_t iterations = 3U;

    for (uint32_t iteration = 1U; iteration <= iterations; ++iteration) {
        const uint32_t previous_start_tick = absolute_start_tick;
        uint32_t missed = 0U;
        absolute_start_tick = delay_until_next_start(
            &last_wake_tick,
            absolute_start_tick,
            work_ticks,
            period_ticks,
            &missed);
        total_missed += missed;
        relative_start_tick += work_ticks + period_ticks;
        assert(relative_start_tick == iteration * (work_ticks + period_ticks));

        if (work_ticks <= period_ticks) {
            assert(absolute_start_tick == iteration * period_ticks);
            assert(missed == 0U);
        } else {
            assert(absolute_start_tick > previous_start_tick + work_ticks);
            assert(absolute_start_tick % period_ticks == 0U);
            assert(missed > 0U);
        }
    }

    if (work_ticks > 0U && work_ticks < period_ticks) {
        assert(relative_start_tick > iterations * period_ticks);
    }
    if (work_ticks > period_ticks) {
        assert(total_missed > 0U);
        assert(absolute_start_tick % period_ticks == 0U);
        assert(relative_start_tick != absolute_start_tick);
    }
}

static void test_characterization_relative_and_absolute_periodic_work(void)
{
    const uint32_t period_ticks = pdMS_TO_TICKS(1000U);
    assert(period_ticks == 100U);
    assert_periodic_work_contract(0U, period_ticks);
    assert_periodic_work_contract(period_ticks / 2U, period_ticks);
    assert_periodic_work_contract(period_ticks, period_ticks);
    assert_periodic_work_contract(period_ticks + 1U, period_ticks);
    assert_periodic_work_contract((2U * period_ticks) + (period_ticks / 2U), period_ticks);
}

int main(void)
{
    test_characterization_pdms_uses_configured_100_hz_tick();
    test_mutation_only_deadline_boundaries_and_tick_wrap();
    test_characterization_relative_and_absolute_periodic_work();
    return 0;
}
