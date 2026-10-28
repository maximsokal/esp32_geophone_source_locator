#include <assert.h>
#include <math.h>
#include <string.h>

#include "app_pod_status_sensor_values_policy.h"

static nrf_remote_sensor_cache_t make_cache(uint32_t valid_mask, uint32_t fault_mask)
{
    nrf_remote_sensor_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.response_valid = true;
    cache.topology_current = true;
    cache.sensor.valid_mask = valid_mask;
    cache.sensor.fault_mask = fault_mask;
    cache.sensor.env_temperature_milli_c = 24250;
    cache.sensor.humidity_milli_percent = 48125U;
    cache.sensor.gyro_mdps[0] = 3000;
    cache.sensor.gyro_mdps[1] = 4000;
    cache.sensor.gyro_mdps[2] = 0;
    return cache;
}

static bool near(float value, float expected)
{
    return fabsf(value - expected) < 0.001f;
}

static void test_null_stale_or_unanswered_cache_gives_nothing(void)
{
    app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(NULL);
    assert(!values.temperature_valid && !values.humidity_valid && !values.gyro_valid);

    nrf_remote_sensor_cache_t cache = make_cache(
        NRF_UPSTREAM_SENSOR_VALID_ENV | NRF_UPSTREAM_SENSOR_VALID_IMU, 0U);
    cache.response_valid = false;
    values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(!values.temperature_valid && !values.gyro_valid);

    cache.response_valid = true;
    cache.topology_current = false;
    values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(!values.temperature_valid && !values.gyro_valid);
}

static void test_each_domain_is_valid_only_on_its_own_bit(void)
{
    nrf_remote_sensor_cache_t cache = make_cache(NRF_UPSTREAM_SENSOR_VALID_ENV, 0U);
    app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(values.temperature_valid && near(values.temperature_c, 24.25f));
    assert(values.humidity_valid && near(values.humidity_percent, 48.125f));
    assert(!values.gyro_valid && values.gyro_rate_dps == 0.0f);

    cache = make_cache(NRF_UPSTREAM_SENSOR_VALID_IMU, 0U);
    values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(!values.temperature_valid && !values.humidity_valid);
    assert(values.gyro_valid && near(values.gyro_rate_dps, 5.0f));
}

static void test_faulted_domain_is_not_published(void)
{
    nrf_remote_sensor_cache_t cache = make_cache(
        NRF_UPSTREAM_SENSOR_VALID_ENV | NRF_UPSTREAM_SENSOR_VALID_IMU,
        NRF_UPSTREAM_SENSOR_VALID_IMU);
    app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(values.temperature_valid);
    assert(!values.gyro_valid && values.gyro_rate_dps == 0.0f);

    cache = make_cache(
        NRF_UPSTREAM_SENSOR_VALID_ENV | NRF_UPSTREAM_SENSOR_VALID_IMU,
        NRF_UPSTREAM_SENSOR_VALID_ENV);
    values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(!values.temperature_valid && !values.humidity_valid);
    assert(values.gyro_valid);
}

static void test_calibrated_gyro_subtracts_the_stored_bias(void)
{
    nrf_remote_sensor_cache_t cache = make_cache(NRF_UPSTREAM_SENSOR_VALID_IMU, 0U);
    cache.sensor.gyro_bias_mdps[0] = 3000;
    cache.sensor.gyro_bias_mdps[1] = 4000;

    /* Not calibrated: the bias is not trusted yet, raw magnitude is shown. */
    cache.sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED;
    assert(near(app_pod_status_bridge_compute_sensor_values(&cache).gyro_rate_dps, 5.0f));

    cache.sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    assert(near(app_pod_status_bridge_compute_sensor_values(&cache).gyro_rate_dps, 0.0f));
}

static void test_humidity_above_hundred_is_clamped(void)
{
    nrf_remote_sensor_cache_t cache = make_cache(NRF_UPSTREAM_SENSOR_VALID_ENV, 0U);
    cache.sensor.humidity_milli_percent = 102400U;
    const app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(values.humidity_valid && values.humidity_percent == 100.0f);
}

static void test_bmp280_humidity_is_unavailable(void)
{
    nrf_remote_sensor_cache_t cache = make_cache(NRF_UPSTREAM_SENSOR_VALID_ENV, 0U);
    cache.sensor.humidity_milli_percent = UINT32_MAX;
    const app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(values.temperature_valid && near(values.temperature_c, 24.25f));
    assert(!values.humidity_valid && values.humidity_percent == 0.0f);
}

int main(void)
{
    test_null_stale_or_unanswered_cache_gives_nothing();
    test_each_domain_is_valid_only_on_its_own_bit();
    test_faulted_domain_is_not_published();
    test_calibrated_gyro_subtracts_the_stored_bias();
    test_humidity_above_hundred_is_clamped();
    test_bmp280_humidity_is_unavailable();
    return 0;
}
