#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define INA3221_STATUS_LOG_HEARTBEAT_MS 60000U
#define INA3221_STATUS_LOG_VOLTAGE_DELTA_MV 50
#define INA3221_STATUS_LOG_CURRENT_DELTA_MA 30
#define INA3221_STATUS_LOG_RAIL_COUNT 3U


typedef struct
{
    bool has_logged;
    uint32_t last_log_ms;
    uint32_t power_state;
    uint32_t power_source;
    bool usb_present;
    bool battery_present;
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT];
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT];
} ina3221_status_log_state_t;


static inline bool ina3221_status_log_should_emit(
    ina3221_status_log_state_t *state,
    uint32_t now_ms,
    uint32_t power_state,
    uint32_t power_source,
    bool usb_present,
    bool battery_present,
    const int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT],
    const int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT]
)
{
    size_t i;
    bool changed;
    bool heartbeat_due;

    changed = !state->has_logged;
    if (!changed) {
        changed =
            state->power_state != power_state ||
            state->power_source != power_source ||
            state->usb_present != usb_present ||
            state->battery_present != battery_present;
    }

    for (i = 0U; !changed && i < INA3221_STATUS_LOG_RAIL_COUNT; ++i) {
        int64_t voltage_delta;
        int64_t current_delta;

        voltage_delta = (int64_t)rail_mv[i] - state->rail_mv[i];
        current_delta = (int64_t)rail_ma[i] - state->rail_ma[i];
        if (voltage_delta < 0) {
            voltage_delta = -voltage_delta;
        }
        if (current_delta < 0) {
            current_delta = -current_delta;
        }
        changed =
            voltage_delta >= INA3221_STATUS_LOG_VOLTAGE_DELTA_MV ||
            current_delta >= INA3221_STATUS_LOG_CURRENT_DELTA_MA;
    }

    heartbeat_due = state->has_logged &&
        (uint32_t)(now_ms - state->last_log_ms) >=
            INA3221_STATUS_LOG_HEARTBEAT_MS;
    if (!changed && !heartbeat_due) {
        return false;
    }

    state->has_logged = true;
    state->last_log_ms = now_ms;
    state->power_state = power_state;
    state->power_source = power_source;
    state->usb_present = usb_present;
    state->battery_present = battery_present;
    for (i = 0U; i < INA3221_STATUS_LOG_RAIL_COUNT; ++i) {
        state->rail_mv[i] = rail_mv[i];
        state->rail_ma[i] = rail_ma[i];
    }

    return true;
}
