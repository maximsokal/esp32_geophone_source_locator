#define main status_baseline_main
#include "device_status_aggregation_test.c"
#undef main

#include <float.h>
#include <stdio.h>

static void test_bad_sensor_values_do_not_replace_status(void)
{
    device_status_snapshot_t before, after;
    assert(device_status_get_snapshot(&before) == ESP_OK);
    const float bad[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0U; i < 3U; ++i) {
        assert(device_status_set_power(bad[i], 1.0f, true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power(5.0f, bad[i], true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power_channel(0U, bad[i], 1.0f, true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power_channel(0U, 5.0f, bad[i], true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_pod_gyro_rate(0U, bad[i], true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_pod_environment(0U, bad[i], true, 50.0f, true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_pod_environment(0U, 20.0f, true, bad[i], true) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power_battery(bad[i], 0.1f, 50.0f, true, true,
            false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power_battery(3.8f, bad[i], 50.0f, true, true,
            false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_ERR_INVALID_ARG);
        assert(device_status_set_power_battery(3.8f, 0.1f, bad[i], true, true,
            false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_ERR_INVALID_ARG);
        for (unsigned field = 0U; field < 3U; ++field) {
            const float t = field == 0U ? bad[i] : 20.0f;
            const float h = field == 1U ? bad[i] : 50.0f;
            const float p = field == 2U ? bad[i] : 100000.0f;
            assert(device_status_set_environment(t, h, p, true) == ESP_ERR_INVALID_ARG);
            assert(device_status_set_pod_imu(0U, field == 0U ? bad[i] : 0.0f,
                field == 1U ? bad[i] : 0.0f, field == 2U ? bad[i] : 0.0f, true) == ESP_ERR_INVALID_ARG);
            assert(device_status_set_pod_barometer(0U, field == 0U ? bad[i] : 100000.0f,
                field == 1U ? bad[i] : 0.0f, field == 2U ? bad[i] : 101325.0f, true) == ESP_ERR_INVALID_ARG);
        }
        for (unsigned field = 0U; field < 5U; ++field) {
            float values[] = {-1.0f, 1.0f, 2.0f, 0.5f, -3.0f};
            values[field] = bad[i];
            assert(device_status_set_pod_geophone(0U, values[0], values[1], values[2],
                values[3], values[4], true) == ESP_ERR_INVALID_ARG);
        }
    }
    assert(device_status_set_power(FLT_MAX, FLT_MAX, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_channel(0U, FLT_MAX, FLT_MAX, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_environment(20.0f, 100.1f, 100000.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_environment(20.0f, -0.1f, 100000.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_environment(20.0f, 50.0f, 0.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_barometer(0U, 0.0f, 0.0f, 101325.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_barometer(0U, 100000.0f, 0.0f, 0.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_battery(3.8f, 0.1f, -0.1f, true, true,
        false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_battery(3.8f, 0.1f, 100.1f, true, true,
        false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_battery(0.0f, 0.0f, 50.0f, false, true,
        false, false, DEVICE_POWER_SOURCE_UNKNOWN) == ESP_ERR_INVALID_ARG);
    for (unsigned field = 1U; field <= 3U; ++field) {
        float values[] = {-1.0f, 1.0f, 2.0f, 0.5f, -3.0f};
        values[field] = -0.1f;
        assert(device_status_set_pod_geophone(0U, values[0], values[1], values[2],
            values[3], values[4], true) == ESP_ERR_INVALID_ARG);
    }
    assert(device_status_get_snapshot(&after) == ESP_OK);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
    puts("SIGNAL_CASE STATUS.REJECT_INVALID PASS");
}

static void test_invalid_flags_clear_numeric_payloads(void)
{
    assert(device_status_set_power(NAN, INFINITY, false) == ESP_OK);
    assert(device_status_set_power_channel(0U, NAN, INFINITY, false) == ESP_OK);
    assert(device_status_set_environment(NAN, INFINITY, NAN, false) == ESP_OK);
    assert(device_status_set_pod_imu(0U, NAN, INFINITY, NAN, false) == ESP_OK);
    assert(device_status_set_pod_barometer(0U, NAN, INFINITY, NAN, false) == ESP_OK);
    assert(device_status_set_pod_geophone(0U, NAN, INFINITY, NAN, INFINITY, NAN, false) == ESP_OK);
    assert(device_status_set_pod_gyro_rate(0U, INFINITY, false) == ESP_OK);
    assert(device_status_set_pod_environment(0U, NAN, false, INFINITY, false) == ESP_OK);
    assert(device_status_set_power_battery(NAN, INFINITY, NAN, false, false,
        false, false, DEVICE_POWER_SOURCE_UNKNOWN) == ESP_OK);
    device_status_snapshot_t value;
    assert(device_status_get_snapshot(&value) == ESP_OK);
    assert(!value.power.valid && value.power.voltage_v == 0.0f && value.power.current_a == 0.0f);
    assert(!value.power.channels[0].valid && value.power.channels[0].power_w == 0.0f);
    assert(!value.environment.valid && value.environment.pressure_pa == 0.0f);
    assert(!value.pods[0].imu.valid && value.pods[0].imu.roll_deg == 0.0f);
    assert(!value.pods[0].imu.gyro_valid && value.pods[0].imu.gyro_rate_dps == 0.0f);
    assert(!value.pods[0].environment.valid && value.pods[0].environment.temperature_c == 0.0f &&
        value.pods[0].environment.humidity_percent == 0.0f);
    assert(!value.pods[0].barometer.valid && value.pods[0].barometer.pressure_pa == 0.0f);
    assert(!value.pods[0].geophone.valid && value.pods[0].geophone.snr_db == 0.0f);
    assert(!value.power.battery_present && !value.power.battery_percent_valid);
    assert(value.power.battery_voltage_v == 0.0f && value.power.battery_current_a == 0.0f &&
        value.power.battery_percent_estimate == 0.0f);
    assert(device_status_set_power_battery(3.8f, -0.1f, NAN, true, false,
        false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_OK);
    assert(device_status_get_snapshot(&value) == ESP_OK);
    assert(value.power.battery_present && !value.power.battery_percent_valid &&
        value.power.battery_percent_estimate == 0.0f);
    puts("SIGNAL_CASE STATUS.INVALID_FLAGS PASS");
}

static void test_status_enum_and_presence_domains(void)
{
    for (unsigned value = 0U; value <= UINT8_MAX; ++value) {
        assert(device_status_set_pod_state(0U, (pod_state_t)value) ==
            (value <= POD_STATE_ERROR ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_power_state((device_power_state_t)value, false,
            DEVICE_LOW_BATTERY_NORMAL, 0U) ==
            (value <= DEVICE_POWER_STATE_BATTERY_CRITICAL ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_battery_soc_quality((device_battery_soc_quality_t)value) ==
            (value <= DEVICE_BATTERY_SOC_FULL_ANCHORED ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_power_state(DEVICE_POWER_STATE_UNKNOWN, false,
            (device_low_battery_phase_t)value, 0U) ==
            (value <= DEVICE_LOW_BATTERY_SHUTDOWN ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_power_channel_overcurrent(0U,
            (device_power_overcurrent_t)value, false) ==
            (value <= DEVICE_POWER_OVERCURRENT_CRITICAL ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_power_battery(0.0f, 0.0f, 0.0f, false, false,
            false, false, (device_power_source_t)value) ==
            (value <= DEVICE_POWER_SOURCE_USB ? ESP_OK : ESP_ERR_INVALID_ARG));
        assert(device_status_set_pod_height_reference(0U,
            (device_height_reference_state_t)value, 0U, 0) ==
            (value <= DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID ? ESP_OK : ESP_ERR_INVALID_ARG));
        const device_main_status_t main_value = {.height_reference = {
            .state = (device_main_height_reference_state_t)value}};
        assert(device_status_set_main(&main_value) ==
            (value <= DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID ? ESP_OK : ESP_ERR_INVALID_ARG));
    }
    assert(device_status_set_pod_state(0U, (pod_state_t)-1) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_state((device_power_state_t)-1, false,
        DEVICE_LOW_BATTERY_NORMAL, 0U) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_channel_overcurrent(0U,
        (device_power_overcurrent_t)-1, false) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_battery_soc_quality((device_battery_soc_quality_t)-1) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_state(DEVICE_POWER_STATE_UNKNOWN, false,
        (device_low_battery_phase_t)-1, 0U) == ESP_ERR_INVALID_ARG);
    for (unsigned flags = 0U; flags < 16U; ++flags) {
        const device_main_status_t main_value = {
            .bmp280_present = (flags & 1U) != 0U, .bmp280_valid = (flags & 2U) != 0U,
            .aht20_present = (flags & 4U) != 0U, .aht20_valid = (flags & 8U) != 0U,
            .pressure_pa = 100000U, .humidity_mpct = 50000U,
        };
        const bool coherent = (!main_value.bmp280_valid || main_value.bmp280_present) &&
            (!main_value.aht20_valid || main_value.aht20_present);
        assert(device_status_set_main(&main_value) == (coherent ? ESP_OK : ESP_ERR_INVALID_ARG));
    }
    puts("SIGNAL_CASE STATUS.ENUMS_PRESENCE PASS bytes=256 flag_sets=16");
}

int main(void)
{
    assert(status_baseline_main() == 0);
    test_bad_sensor_values_do_not_replace_status();
    test_invalid_flags_clear_numeric_payloads();
    test_status_enum_and_presence_domains();
    return 0;
}
