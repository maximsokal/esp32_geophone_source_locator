#include "ina3221_low_battery.h"

#include <stddef.h>


void ina3221_low_battery_init(ina3221_low_battery_t *policy, uint32_t grace_ms)
{
    if (policy == NULL) {
        return;
    }
    *policy = (ina3221_low_battery_t) {
        .phase = INA3221_LOW_BATTERY_NORMAL,
        .grace_started_ms = 0U,
        .grace_ms = grace_ms,
    };
}


ina3221_low_battery_action_t ina3221_low_battery_update(
    ina3221_low_battery_t *policy,
    ina3221_power_ux_state_t power_state,
    uint64_t now_ms)
{
    if (policy == NULL) {
        return INA3221_LOW_BATTERY_ACTION_NONE;
    }

    switch (policy->phase) {
        case INA3221_LOW_BATTERY_SHUTDOWN:
            /* Irreversible: the safe shutdown is already running. */
            return INA3221_LOW_BATTERY_ACTION_NONE;

        case INA3221_LOW_BATTERY_GRACE:
            if (ina3221_power_ux_usb_present(power_state)) {
                policy->phase = INA3221_LOW_BATTERY_NORMAL;
                return INA3221_LOW_BATTERY_ACTION_CANCELLED;
            }
            /* Unknown telemetry or a partial recovery does not stop the
             * countdown: only proven USB power can. */
            if (now_ms - policy->grace_started_ms >= policy->grace_ms) {
                policy->phase = INA3221_LOW_BATTERY_SHUTDOWN;
                return INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN;
            }
            return INA3221_LOW_BATTERY_ACTION_NONE;

        case INA3221_LOW_BATTERY_NORMAL:
        case INA3221_LOW_BATTERY_WARNING:
        default:
            if (power_state == INA3221_POWER_UX_BATTERY_CRITICAL) {
                policy->phase = INA3221_LOW_BATTERY_GRACE;
                policy->grace_started_ms = now_ms;
                if (policy->grace_ms == 0U) {
                    policy->phase = INA3221_LOW_BATTERY_SHUTDOWN;
                    return INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN;
                }
                return INA3221_LOW_BATTERY_ACTION_START_GRACE;
            }
            if (power_state == INA3221_POWER_UX_BATTERY_LOW) {
                if (policy->phase != INA3221_LOW_BATTERY_WARNING) {
                    policy->phase = INA3221_LOW_BATTERY_WARNING;
                    return INA3221_LOW_BATTERY_ACTION_WARN;
                }
                return INA3221_LOW_BATTERY_ACTION_NONE;
            }
            /* BATTERY, USB or UNKNOWN: a warning clears only on a real
             * improvement, not on missing telemetry. */
            if (power_state != INA3221_POWER_UX_UNKNOWN) {
                policy->phase = INA3221_LOW_BATTERY_NORMAL;
            }
            return INA3221_LOW_BATTERY_ACTION_NONE;
    }
}


uint32_t ina3221_low_battery_remaining_s(
    const ina3221_low_battery_t *policy,
    uint64_t now_ms)
{
    if (policy == NULL || policy->phase != INA3221_LOW_BATTERY_GRACE) {
        return 0U;
    }
    const uint64_t elapsed = now_ms - policy->grace_started_ms;
    if (elapsed >= policy->grace_ms) {
        return 0U;
    }
    return (uint32_t)((policy->grace_ms - elapsed + 999U) / 1000U);
}


const char *ina3221_low_battery_phase_name(ina3221_low_battery_phase_t phase)
{
    switch (phase) {
        case INA3221_LOW_BATTERY_NORMAL: return "normal";
        case INA3221_LOW_BATTERY_WARNING: return "warning";
        case INA3221_LOW_BATTERY_GRACE: return "grace";
        case INA3221_LOW_BATTERY_SHUTDOWN: return "shutdown";
        default: return "unknown";
    }
}
