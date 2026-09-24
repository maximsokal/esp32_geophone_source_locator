#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ina3221_power_ux.h"

/*
 * MAX-183: what the device does about a low battery.
 *
 *   NORMAL   -> WARNING    stable BATTERY_LOW: persistent warning only.
 *   any      -> GRACE      stable BATTERY_CRITICAL: "CONNECT USB" countdown.
 *   GRACE    -> NORMAL     USB became stably present before the countdown
 *                          ended: shutdown CANCELLED (reported once).
 *   GRACE    -> SHUTDOWN   countdown ended: request the existing safe
 *                          shutdown (recorder finalised, SD flushed, OFF
 *                          pulse). Irreversible: once requested it is never
 *                          cancelled, even if USB appears afterwards.
 *
 * The countdown only covers the time needed to plug USB in; the UPS board's
 * own cutoff must still be later than grace + shutdown duration, which the
 * CRITICAL threshold (MAX-180) has to guarantee once calibrated (MAX-184).
 */

typedef enum
{
    INA3221_LOW_BATTERY_NORMAL = 0,
    INA3221_LOW_BATTERY_WARNING,
    INA3221_LOW_BATTERY_GRACE,
    INA3221_LOW_BATTERY_SHUTDOWN,

} ina3221_low_battery_phase_t;

typedef enum
{
    INA3221_LOW_BATTERY_ACTION_NONE = 0,
    INA3221_LOW_BATTERY_ACTION_WARN,
    INA3221_LOW_BATTERY_ACTION_START_GRACE,
    INA3221_LOW_BATTERY_ACTION_CANCELLED,
    INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN,

} ina3221_low_battery_action_t;

typedef struct
{
    ina3221_low_battery_phase_t phase;
    uint64_t grace_started_ms;
    uint32_t grace_ms;

} ina3221_low_battery_t;

void ina3221_low_battery_init(ina3221_low_battery_t *policy, uint32_t grace_ms);

/* Feed the stable power state once per sample. Returns the one-shot action
 * for this step (NONE when nothing changed). */
ina3221_low_battery_action_t ina3221_low_battery_update(
    ina3221_low_battery_t *policy,
    ina3221_power_ux_state_t power_state,
    uint64_t now_ms);

/* Seconds left in GRACE (0 outside GRACE). */
uint32_t ina3221_low_battery_remaining_s(
    const ina3221_low_battery_t *policy,
    uint64_t now_ms);

const char *ina3221_low_battery_phase_name(ina3221_low_battery_phase_t phase);
