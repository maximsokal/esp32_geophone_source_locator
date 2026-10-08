#include "ina3221_status_log_policy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>


static void baseline(
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
    memset(state, 0, sizeof(*state));
    assert(ina3221_status_log_should_emit(
        state, now_ms, power_state, power_source, usb_present,
        battery_present, rail_mv, rail_ma
    ));
}


static void test_first_change_and_heartbeat(void)
{
    ina3221_status_log_state_t state = {0};
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT] = { 3700, 5000, 5000 };
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT] = { 100, 200, 300 };

    assert(ina3221_status_log_should_emit(
        &state, 0U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    assert(!ina3221_status_log_should_emit(
        &state, 10000U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    assert(!ina3221_status_log_should_emit(
        &state, 59999U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    assert(ina3221_status_log_should_emit(
        &state, 60000U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    assert(!ina3221_status_log_should_emit(
        &state, 119999U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    assert(ina3221_status_log_should_emit(
        &state, 120000U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
}


static void test_each_state_field_logs_immediately(void)
{
    unsigned field;
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT] = { 3700, 5000, 5000 };
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT] = { 100, 200, 300 };

    for (field = 0U; field < 4U; ++field) {
        ina3221_status_log_state_t state = {0};
        uint32_t power_state = 1U;
        uint32_t power_source = 2U;
        bool usb_present = true;
        bool battery_present = true;

        baseline(&state, 10U, power_state, power_source, usb_present,
                 battery_present, rail_mv, rail_ma);
        if (field == 0U) {
            power_state++;
        } else if (field == 1U) {
            power_source++;
        } else if (field == 2U) {
            usb_present = false;
        } else {
            battery_present = false;
        }
        assert(ina3221_status_log_should_emit(
            &state, 11U, power_state, power_source, usb_present,
            battery_present, rail_mv, rail_ma
        ));
    }
}


static void test_voltage_and_current_thresholds_per_rail(void)
{
    size_t rail;
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT] = { 3700, 5000, 5000 };
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT] = { 100, 200, 300 };

    for (rail = 0U; rail < INA3221_STATUS_LOG_RAIL_COUNT; ++rail) {
        ina3221_status_log_state_t state = {0};
        baseline(&state, 100U, 1U, 2U, true, true, rail_mv, rail_ma);
        rail_mv[rail] += 49;
        assert(!ina3221_status_log_should_emit(
            &state, 101U, 1U, 2U, true, true, rail_mv, rail_ma
        ));
        rail_mv[rail]++;
        assert(ina3221_status_log_should_emit(
            &state, 102U, 1U, 2U, true, true, rail_mv, rail_ma
        ));
        rail_mv[rail] -= 50;

        memset(&state, 0, sizeof(state));
        baseline(&state, 200U, 1U, 2U, true, true, rail_mv, rail_ma);
        rail_ma[rail] += 29;
        assert(!ina3221_status_log_should_emit(
            &state, 201U, 1U, 2U, true, true, rail_mv, rail_ma
        ));
        rail_ma[rail]++;
        assert(ina3221_status_log_should_emit(
            &state, 202U, 1U, 2U, true, true, rail_mv, rail_ma
        ));
        rail_ma[rail] -= 30;
    }
}


static void test_voltage_drift_compares_with_last_logged_snapshot(void)
{
    ina3221_status_log_state_t state = {0};
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT] = { 3700, 5000, 5000 };
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT] = { 100, 200, 300 };

    baseline(&state, 0U, 1U, 2U, true, true, rail_mv, rail_ma);
    rail_mv[0] += 30;
    assert(!ina3221_status_log_should_emit(
        &state, 5000U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
    rail_mv[0] += 30;
    assert(ina3221_status_log_should_emit(
        &state, 10000U, 1U, 2U, true, true, rail_mv, rail_ma
    ));
}


static void test_heartbeat_handles_uint32_wraparound(void)
{
    ina3221_status_log_state_t state = {0};
    int32_t rail_mv[INA3221_STATUS_LOG_RAIL_COUNT] = { 3700, 5000, 5000 };
    int32_t rail_ma[INA3221_STATUS_LOG_RAIL_COUNT] = { 100, 200, 300 };
    const uint32_t start_ms = UINT32_MAX - 1000U;

    baseline(&state, start_ms, 1U, 2U, true, true, rail_mv, rail_ma);
    assert(!ina3221_status_log_should_emit(
        &state, (uint32_t)(start_ms + 59999U), 1U, 2U, true, true,
        rail_mv, rail_ma
    ));
    assert(ina3221_status_log_should_emit(
        &state, (uint32_t)(start_ms + 60000U), 1U, 2U, true, true,
        rail_mv, rail_ma
    ));
}


int main(void)
{
    test_first_change_and_heartbeat();
    test_each_state_field_logs_immediately();
    test_voltage_and_current_thresholds_per_rail();
    test_voltage_drift_compares_with_last_logged_snapshot();
    test_heartbeat_handles_uint32_wraparound();
    return 0;
}
