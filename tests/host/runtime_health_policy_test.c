#include <assert.h>
#include <stdint.h>

#include "app_stop_timeout_config.h"
#include "runtime_health_policy.h"


static void test_live_heartbeat_does_not_hide_frozen_progress(void)
{
    const runtime_health_observation_t frozen = {
        .observed = true,
        .heartbeat = 900U,
        .progress = 42U,
        .last_progress_ms = 1000U,
        .phase_started_ms = 1000U,
    };
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        3500U
    ) == RUNTIME_HEALTH_STALE_PROGRESS);
}


static void test_allowed_wait_is_quiet_until_its_deadline(void)
{
    const runtime_health_observation_t waiting = {
        .observed = true,
        .heartbeat = 1U,
        .progress = 0U,
        .last_progress_ms = 0U,
        .phase_started_ms = 1000U,
    };
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_STORAGE,
        RUNTIME_HEALTH_PHASE_WAITING,
        &waiting,
        5999U
    ) == RUNTIME_HEALTH_OK);
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_STORAGE,
        RUNTIME_HEALTH_PHASE_WAITING,
        &waiting,
        6000U
    ) == RUNTIME_HEALTH_DEADLINE_EXCEEDED);
}


static void test_progress_deadline_boundaries(void)
{
    const runtime_health_observation_t frozen = {
        .observed = true,
        .heartbeat = 50U,
        .progress = 7U,
        .last_progress_ms = 2000U,
        .phase_started_ms = 2000U,
    };
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        6999U
    ) == RUNTIME_HEALTH_STALE_PROGRESS);
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        7000U
    ) == RUNTIME_HEALTH_DEADLINE_EXCEEDED);
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        7001U
    ) == RUNTIME_HEALTH_DEADLINE_EXCEEDED);
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_RECORDER,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        1999U
    ) == RUNTIME_HEALTH_OK);
    assert(runtime_health_policy_deadline_ms(
        RUNTIME_HEALTH_TASK_CONTROL,
        RUNTIME_HEALTH_PHASE_STOP_FLUSH
    ) == APP_STOP_TIMEOUT_RECORDING_MS);

    const runtime_health_observation_t stop_stage = {
        .observed = true,
        .heartbeat = 50U,
        .progress = 7U,
        .last_progress_ms = 100U,
        .phase_started_ms = 10000U,
    };
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_RECORDER,
        RUNTIME_HEALTH_PHASE_STOP_FLUSH,
        &stop_stage,
        10001U
    ) == RUNTIME_HEALTH_OK);
}


static void test_reaction_order_and_hard_failure_gate(void)
{
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_OK, false, false, false
    ) == RUNTIME_HEALTH_REACTION_DIAGNOSE);
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_STALE_PROGRESS, true, false, false
    ) == RUNTIME_HEALTH_REACTION_DIAGNOSE);
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_DEADLINE_EXCEEDED, true, false, false
    ) == RUNTIME_HEALTH_REACTION_MARK_INCOMPLETE);
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_DEADLINE_EXCEEDED, true, true, false
    ) == RUNTIME_HEALTH_REACTION_SAFE_STOP);
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_DEADLINE_EXCEEDED, false, false, false
    ) == RUNTIME_HEALTH_REACTION_SAFE_STOP);
    for (runtime_health_result_t result = RUNTIME_HEALTH_OK;
         result <= RUNTIME_HEALTH_DEADLINE_EXCEEDED;
         result = (runtime_health_result_t)(result + 1)) {
        assert(runtime_health_policy_reaction(
            result, true, false, false
        ) != RUNTIME_HEALTH_REACTION_REBOOT);
    }
    assert(runtime_health_policy_reaction(
        RUNTIME_HEALTH_DEADLINE_EXCEEDED, true, false, true
    ) == RUNTIME_HEALTH_REACTION_REBOOT);
}


static void test_missing_expected_task_blocks_completeness(void)
{
    const char *const expected[] = { "acquisition", "recorder", "storage" };
    const char *const partial[] = { "acquisition", "recorder" };
    const char *const complete[] = { "storage", "recorder", "acquisition" };
    assert(runtime_health_policy_check_task_set(
        expected, 3U, partial, 2U
    ) == RUNTIME_HEALTH_TASK_SET_BLOCKED);
    assert(runtime_health_policy_check_task_set(
        expected, 3U, complete, 3U
    ) == RUNTIME_HEALTH_TASK_SET_COMPLETE);
}


static void test_memory_watch_reports_a_loss_with_the_value_it_came_from(void)
{
    runtime_memory_watch_t watch = {0};
    uint32_t from = 0U;

    assert(!runtime_memory_watch_update(&watch, 6071U, 2048U, &from));        /* the first sample only arms it */
    assert(!runtime_memory_watch_update(&watch, 6071U, 2048U, &from));
    assert(!runtime_memory_watch_update(&watch, 4600U, 2048U, &from));        /* the 1.5 KB reserve: quiet */
    assert(!runtime_memory_watch_update(&watch, 4024U, 2048U, &from));        /* 2047 under the reference */
    assert(runtime_memory_watch_update(&watch, 4023U, 2048U, &from));         /* exactly the threshold */
    assert(from == 6071U);
    assert(!runtime_memory_watch_update(&watch, 4023U, 2048U, &from));        /* reported once, not every second */

    /* the 5.9 KB loss of the bench log, in two steps, and the return of the memory */
    assert(runtime_memory_watch_update(&watch, 175U, 2048U, &from));
    assert(from == 4023U);
    assert(runtime_memory_watch_update(&watch, 5743U, 2048U, &from));
    assert(from == 175U);
}


static void test_memory_watch_ignores_bad_arguments(void)
{
    runtime_memory_watch_t watch = {0};
    uint32_t from = 77U;
    assert(!runtime_memory_watch_update(NULL, 1U, 2048U, &from));
    assert(!runtime_memory_watch_update(&watch, 6000U, 0U, &from));           /* arms */
    assert(!runtime_memory_watch_update(&watch, 0U, 0U, &from));              /* a zero threshold reports nothing */
    assert(from == 77U);
    assert(runtime_memory_watch_update(&watch, 5000U, 1U, NULL));             /* the origin is optional */
}


int main(void)
{
    test_live_heartbeat_does_not_hide_frozen_progress();
    test_allowed_wait_is_quiet_until_its_deadline();
    test_progress_deadline_boundaries();
    test_reaction_order_and_hard_failure_gate();
    test_missing_expected_task_blocks_completeness();
    test_memory_watch_reports_a_loss_with_the_value_it_came_from();
    test_memory_watch_ignores_bad_arguments();
    return 0;
}
