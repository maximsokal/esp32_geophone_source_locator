#define main measured_values_baseline_main
#include "app_pod_status_sensor_values_policy_test.c"
#undef main

#include <limits.h>
#include <stdio.h>

static void test_all_measurement_domain_and_current_flags(void)
{
    for (unsigned valid = 0U; valid <= UINT8_MAX; ++valid) {
        for (unsigned fault = 0U; fault <= UINT8_MAX; ++fault) {
            for (unsigned flags = 0U; flags < 4U; ++flags) {
                nrf_remote_sensor_cache_t cache = make_cache(valid, fault);
                cache.response_valid = (flags & 1U) != 0U;
                cache.topology_current = (flags & 2U) != 0U;
                const app_pod_status_sensor_values_t result = app_pod_status_bridge_compute_sensor_values(&cache);
                const bool imu = flags == 3U && (valid & 1U) != 0U && (fault & 1U) == 0U;
                const bool env = flags == 3U && (valid & 2U) != 0U && (fault & 2U) == 0U;
                assert(result.gyro_valid == imu && result.temperature_valid == env && result.humidity_valid == env);
                assert(result.gyro_rate_dps == (imu ? 5.0f : 0.0f));
                assert(result.temperature_c == (env ? 24.25f : 0.0f));
                assert(result.humidity_percent == (env ? 48.125f : 0.0f));
            }
        }
    }
    puts("SIGNAL_CASE MEASURED.MASKS PASS mask_pairs=65536 flag_sets=4");
}

static void test_gyro_integer_extremes_bias_and_environment_boundaries(void)
{
    const int32_t words[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    const int16_t biases[] = {INT16_MIN, 0, INT16_MAX};
    for (unsigned state = 0U; state <= UINT8_MAX; ++state) {
        for (unsigned value = 0U; value < 5U; ++value) {
            for (unsigned bias = 0U; bias < 3U; ++bias) {
                nrf_remote_sensor_cache_t cache = make_cache(3U, 0U);
                cache.sensor.imu_cal_state = (uint8_t)state;
                double rate[3];
                for (unsigned axis = 0U; axis < 3U; ++axis) {
                    cache.sensor.gyro_mdps[axis] = words[(value + axis) % 5U];
                    cache.sensor.gyro_bias_mdps[axis] = biases[(bias + axis) % 3U];
                    rate[axis] = (double)cache.sensor.gyro_mdps[axis];
                    if (state == 1U) rate[axis] -= cache.sensor.gyro_bias_mdps[axis];
                }
                const double expected = hypot(hypot(rate[0], rate[1]), rate[2]) / 1000.0;
                cache.sensor.env_temperature_milli_c = words[value];
                const uint32_t humidity[] = {0U, 1U, 99999U, 100000U, UINT32_MAX};
                cache.sensor.humidity_milli_percent = humidity[value];
                const app_pod_status_sensor_values_t result = app_pod_status_bridge_compute_sensor_values(&cache);
                assert(result.gyro_valid && isfinite(result.gyro_rate_dps) && result.gyro_rate_dps >= 0.0f);
                assert(fabs((double)result.gyro_rate_dps - expected) <= expected * 0.000001 + 0.001);
                assert(result.temperature_valid && isfinite(result.temperature_c));
                assert(result.temperature_c == (float)words[value] / 1000.0f);
                const bool available = humidity[value] != UINT32_MAX;
                assert(result.humidity_valid == available);
                const float expected_humidity = !available ? 0.0f :
                    humidity[value] > 100000U ? 100.0f : (float)humidity[value] / 1000.0f;
                assert(result.humidity_percent == expected_humidity);
            }
        }
    }
    puts("SIGNAL_CASE MEASURED.EXTREMES PASS calibration_states=256");
}

int main(void)
{
    assert(measured_values_baseline_main() == 0);
    test_all_measurement_domain_and_current_flags();
    test_gyro_integer_extremes_bias_and_environment_boundaries();
    return 0;
}
