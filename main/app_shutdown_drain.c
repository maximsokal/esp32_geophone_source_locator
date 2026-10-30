#include "app_shutdown_drain.h"

#include <stddef.h>

app_shutdown_drain_result_t app_shutdown_drain_writes(
    const app_shutdown_drain_ops_t *ops,
    uint32_t timeout_ms,
    uint32_t poll_ms,
    app_shutdown_drain_report_t *report)
{
    if (report != NULL) {
        report->waited_ms = 0U;
        report->stop_requested = false;
    }
    if (ops == NULL || ops->now_ms == NULL || ops->sleep_ms == NULL || ops->writes_pending == NULL ||
        ops->request_stop == NULL || timeout_ms == 0U || poll_ms == 0U) {
        return APP_SHUTDOWN_DRAIN_BAD_ARGUMENT;
    }

    if (!ops->writes_pending(ops->context)) {
        return APP_SHUTDOWN_DRAIN_NOTHING_PENDING;
    }

    /* Stop first, then wait: the flush has to start before the clock does. */
    ops->request_stop(ops->context);
    if (report != NULL) {
        report->stop_requested = true;
    }

    const uint64_t started_ms = ops->now_ms(ops->context);
    for (;;) {
        ops->sleep_ms(ops->context, poll_ms);
        const uint64_t now_ms = ops->now_ms(ops->context);
        const uint64_t elapsed_ms = now_ms >= started_ms ? now_ms - started_ms : 0U;
        if (report != NULL) {
            report->waited_ms = elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
        }
        if (!ops->writes_pending(ops->context)) {
            return APP_SHUTDOWN_DRAIN_COMPLETED;
        }
        if (ops->progress != NULL) {
            ops->progress(ops->context, report != NULL ? report->waited_ms : 0U);
        }
        if (elapsed_ms >= timeout_ms) {
            return APP_SHUTDOWN_DRAIN_TIMED_OUT;
        }
    }
}

app_shutdown_plan_t app_shutdown_plan_after_measurement(bool measurement_shutdown_ok)
{
    return (app_shutdown_plan_t){
        .full_teardown = measurement_shutdown_ok,
        .cut_power = true,
    };
}

const char *app_shutdown_drain_result_name(app_shutdown_drain_result_t result)
{
    switch (result) {
        case APP_SHUTDOWN_DRAIN_NOTHING_PENDING: return "nothing_pending";
        case APP_SHUTDOWN_DRAIN_COMPLETED: return "completed";
        case APP_SHUTDOWN_DRAIN_TIMED_OUT: return "timed_out";
        case APP_SHUTDOWN_DRAIN_BAD_ARGUMENT: return "bad_argument";
        default: return "unknown";
    }
}
