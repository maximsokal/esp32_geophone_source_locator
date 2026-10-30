#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_shutdown_drain.h"

/*
 * Virtual-time model of the SD write queue: `pending_polls` is how many polls the queue still
 * needs before it is empty; a stuck queue never empties.
 */
typedef struct
{
    uint64_t now;
    unsigned pending_polls;
    bool stuck;
    unsigned stop_calls;
    unsigned sleeps;
    unsigned progress_calls;
    uint32_t last_progress_ms;
    bool stop_seen_before_first_sleep;
    bool stop_requested;
    bool pending_changes_after_stop;
    unsigned pending_checks;
} model_t;

static uint64_t fake_now(void *context) { return ((model_t *)context)->now; }

static void fake_sleep(void *context, uint32_t ms)
{
    model_t *model = context;
    if (model->sleeps == 0U) model->stop_seen_before_first_sleep = model->stop_requested;
    ++model->sleeps;
    model->now += ms;
    if (model->stop_requested && model->pending_polls > 0U && !model->stuck) --model->pending_polls;
}

static bool fake_pending(void *context)
{
    model_t *model = context;
    ++model->pending_checks;
    return model->stuck || model->pending_polls > 0U;
}

static void fake_stop(void *context)
{
    model_t *model = context;
    ++model->stop_calls;
    model->stop_requested = true;
}

static void fake_progress(void *context, uint32_t waited_ms)
{
    model_t *model = context;
    assert(waited_ms >= model->last_progress_ms); /* monotonic */
    model->last_progress_ms = waited_ms;
    ++model->progress_calls;
}

static app_shutdown_drain_ops_t ops_for(model_t *model)
{
    return (app_shutdown_drain_ops_t){
        .context = model, .now_ms = fake_now, .sleep_ms = fake_sleep,
        .writes_pending = fake_pending, .request_stop = fake_stop, .progress = fake_progress,
    };
}

static void test_nothing_pending_returns_at_once_without_stopping_anything(void)
{
    model_t model = {.now = 1000U};
    const app_shutdown_drain_ops_t ops = ops_for(&model);
    app_shutdown_drain_report_t report;
    assert(app_shutdown_drain_writes(&ops, 60000U, 100U, &report) == APP_SHUTDOWN_DRAIN_NOTHING_PENDING);
    assert(model.stop_calls == 0U && model.sleeps == 0U);
    assert(!report.stop_requested && report.waited_ms == 0U);
}

static void test_waits_until_the_queue_is_empty_and_only_then_returns(void)
{
    model_t model = {.now = 1000U, .pending_polls = 37U};
    const app_shutdown_drain_ops_t ops = ops_for(&model);
    app_shutdown_drain_report_t report;
    assert(app_shutdown_drain_writes(&ops, 60000U, 100U, &report) == APP_SHUTDOWN_DRAIN_COMPLETED);
    /* The queue is really gone when the call returns: not one poll too early, not one too late. */
    assert(model.pending_polls == 0U);
    assert(model.sleeps == 37U && report.waited_ms == 3700U);
    /* The flush was requested exactly once, before the first wait. */
    assert(model.stop_calls == 1U && report.stop_requested && model.stop_seen_before_first_sleep);
    assert(model.progress_calls == 36U); /* every poll that still found writes pending */
}

static void test_a_stuck_queue_is_given_the_whole_budget_and_no_more(void)
{
    model_t model = {.now = 5000U, .stuck = true};
    const app_shutdown_drain_ops_t ops = ops_for(&model);
    app_shutdown_drain_report_t report;
    assert(app_shutdown_drain_writes(&ops, 60000U, 100U, &report) == APP_SHUTDOWN_DRAIN_TIMED_OUT);
    assert(report.waited_ms >= 60000U && report.waited_ms <= 60000U + 100U);
    assert(model.stop_calls == 1U);

    /* A coarse poll cannot overshoot by more than one poll either. */
    model_t coarse = {.now = 0U, .stuck = true};
    const app_shutdown_drain_ops_t coarse_ops = ops_for(&coarse);
    assert(app_shutdown_drain_writes(&coarse_ops, 1000U, 400U, &report) == APP_SHUTDOWN_DRAIN_TIMED_OUT);
    assert(report.waited_ms >= 1000U && report.waited_ms < 1000U + 400U);
}

static void test_a_queue_that_empties_on_the_last_poll_is_not_called_a_timeout(void)
{
    model_t model = {.now = 0U, .pending_polls = 10U};
    const app_shutdown_drain_ops_t ops = ops_for(&model);
    app_shutdown_drain_report_t report;
    /* The budget ends exactly when the queue is empty: completed wins. */
    assert(app_shutdown_drain_writes(&ops, 1000U, 100U, &report) == APP_SHUTDOWN_DRAIN_COMPLETED);
    assert(report.waited_ms == 1000U);
}

static void test_bad_arguments_touch_nothing(void)
{
    model_t model = {.now = 1U, .pending_polls = 3U};
    app_shutdown_drain_ops_t ops = ops_for(&model);
    app_shutdown_drain_report_t report = {.waited_ms = 7U, .stop_requested = true};
    assert(app_shutdown_drain_writes(NULL, 1000U, 100U, &report) == APP_SHUTDOWN_DRAIN_BAD_ARGUMENT);
    assert(report.waited_ms == 0U && !report.stop_requested);
    assert(app_shutdown_drain_writes(&ops, 0U, 100U, NULL) == APP_SHUTDOWN_DRAIN_BAD_ARGUMENT);
    assert(app_shutdown_drain_writes(&ops, 1000U, 0U, NULL) == APP_SHUTDOWN_DRAIN_BAD_ARGUMENT);
    ops.request_stop = NULL;
    assert(app_shutdown_drain_writes(&ops, 1000U, 100U, NULL) == APP_SHUTDOWN_DRAIN_BAD_ARGUMENT);
    assert(model.stop_calls == 0U && model.sleeps == 0U);
}

static void test_the_device_is_always_switched_off(void)
{
    /* Critical battery: neither a clean nor a failed measurement shutdown may keep the power on. */
    const app_shutdown_plan_t clean = app_shutdown_plan_after_measurement(true);
    const app_shutdown_plan_t failed = app_shutdown_plan_after_measurement(false);
    assert(clean.cut_power && failed.cut_power);
    assert(clean.full_teardown && !failed.full_teardown);

    /* Every drain outcome leads to the same decision: drain, then power off. */
    assert(strcmp(app_shutdown_drain_result_name(APP_SHUTDOWN_DRAIN_TIMED_OUT), "timed_out") == 0);
    assert(strcmp(app_shutdown_drain_result_name(APP_SHUTDOWN_DRAIN_COMPLETED), "completed") == 0);
    assert(strcmp(app_shutdown_drain_result_name(APP_SHUTDOWN_DRAIN_NOTHING_PENDING), "nothing_pending") == 0);
}

int main(void)
{
    test_nothing_pending_returns_at_once_without_stopping_anything();
    test_waits_until_the_queue_is_empty_and_only_then_returns();
    test_a_stuck_queue_is_given_the_whole_budget_and_no_more();
    test_a_queue_that_empties_on_the_last_poll_is_not_called_a_timeout();
    test_bad_arguments_touch_nothing();
    test_the_device_is_always_switched_off();
    puts("app_shutdown_drain tests passed");
    return 0;
}
