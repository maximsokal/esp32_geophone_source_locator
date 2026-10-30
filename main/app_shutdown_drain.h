#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Critical battery (or the button) has asked the device to switch off. Before anything is torn
 * down the data that is already on its way to the SD card is written out:
 *
 *   - the current recording is asked to stop with a full flush (nothing new is started: the Web
 *     UI refuses new commands once the shutdown latch is set, the keypad is already stopped),
 *   - the caller waits until no recording, object or storage write is pending any more,
 *   - the wait is bounded, so the device always switches off in the end.
 *
 * The Web UI and the TFT keep showing "critical battery - stopping" for the whole wait, because
 * the network is stopped only after this returns.
 */

typedef struct
{
    void *context;
    uint64_t (*now_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t ms);
    /* True while a recording is starting/running/stopping or any SD write is still queued. */
    bool (*writes_pending)(void *context);
    /* Ask the running recording to stop and flush; must not block. */
    void (*request_stop)(void *context);
    /* Optional: called after every poll while waiting. */
    void (*progress)(void *context, uint32_t waited_ms);

} app_shutdown_drain_ops_t;

typedef enum
{
    APP_SHUTDOWN_DRAIN_NOTHING_PENDING = 0,
    APP_SHUTDOWN_DRAIN_COMPLETED,
    APP_SHUTDOWN_DRAIN_TIMED_OUT,
    APP_SHUTDOWN_DRAIN_BAD_ARGUMENT,

} app_shutdown_drain_result_t;

typedef struct
{
    uint32_t waited_ms;
    bool stop_requested;

} app_shutdown_drain_report_t;

/* Never waits longer than timeout_ms plus one poll. report may be NULL. */
app_shutdown_drain_result_t app_shutdown_drain_writes(
    const app_shutdown_drain_ops_t *ops,
    uint32_t timeout_ms,
    uint32_t poll_ms,
    app_shutdown_drain_report_t *report);

/*
 * What to do once measurement_service_shutdown() has answered. A failure there (its worker did
 * not stop in time) must not keep the device powered: the battery is already critical. Without
 * a clean worker stop the services that worker uses are left alone and only the power is cut.
 */
typedef struct
{
    bool full_teardown;
    bool cut_power;

} app_shutdown_plan_t;

app_shutdown_plan_t app_shutdown_plan_after_measurement(bool measurement_shutdown_ok);

const char *app_shutdown_drain_result_name(app_shutdown_drain_result_t result);
