#include "ui_perf_policy.h"

#include <assert.h>
#include <stdint.h>


static void test_first_healthy_window_and_heartbeat(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 0U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 10000U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 59999U, 240U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 60000U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 119999U, 240U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 120000U, 240U, 10000000U, 24U, 1000U));
}


static void test_fps_threshold_is_strictly_below_eighty_percent(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 10U, 240U, 10000000U, 24U, 1000U));
    assert(!ui_perf_policy_should_log(&state, 10010U, 192U, 10000000U, 24U, 1000U));
    assert(ui_perf_policy_should_log(&state, 20010U, 191U, 10000000U, 24U, 1000U));
    /* Exactly 80% right after a logged low window is healthy again: no log. */
    assert(!ui_perf_policy_should_log(&state, 30010U, 192U, 10000000U, 24U, 1000U));
}


static void test_render_budget_boundary(void)
{
    ui_perf_policy_state_t state = {0};

    assert(ui_perf_policy_should_log(&state, 0U, 240U, 10000000U, 24U, 41666U));
    assert(!ui_perf_policy_should_log(&state, 10000U, 240U, 10000000U, 24U, 41666U));
    assert(ui_perf_policy_should_log(&state, 20000U, 240U, 10000000U, 24U, 41667U));
}


static void test_heartbeat_handles_uint32_wraparound(void)
{
    ui_perf_policy_state_t state = {0};
    const uint32_t start_ms = UINT32_MAX - 1000U;

    assert(ui_perf_policy_should_log(
        &state, start_ms, 240U, 10000000U, 24U, 1000U
    ));
    assert(!ui_perf_policy_should_log(
        &state, (uint32_t)(start_ms + 59999U), 240U, 10000000U, 24U, 1000U
    ));
    assert(ui_perf_policy_should_log(
        &state, (uint32_t)(start_ms + 60000U), 240U, 10000000U, 24U, 1000U
    ));
}


int main(void)
{
    test_first_healthy_window_and_heartbeat();
    test_fps_threshold_is_strictly_below_eighty_percent();
    test_render_budget_boundary();
    test_heartbeat_handles_uint32_wraparound();
    return 0;
}
