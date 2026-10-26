#include "app_pod_status_sensor_values_policy.h"

#include <math.h>

app_pod_status_sensor_values_t app_pod_status_bridge_compute_sensor_values(
    const nrf_remote_sensor_cache_t *cache)
{
    app_pod_status_sensor_values_t values = {0};
    if (cache == NULL || !cache->response_valid || !cache->topology_current) {
        return values;
    }

    const nrf_upstream_sensor_snapshot_t *sensor = &cache->sensor;
    if ((sensor->valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U &&
        (sensor->fault_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) == 0U) {
        values.temperature_valid = true;
        values.temperature_c = (float)sensor->env_temperature_milli_c / 1000.0f;
        /* The BME280 reports up to 102.4 %; clamp, the field accepts 0..100. */
        const float humidity = (float)sensor->humidity_milli_percent / 1000.0f;
        values.humidity_valid = true;
        values.humidity_percent = humidity > 100.0f ? 100.0f : humidity;
    }

    if ((sensor->valid_mask & NRF_UPSTREAM_SENSOR_VALID_IMU) != 0U &&
        (sensor->fault_mask & NRF_UPSTREAM_SENSOR_VALID_IMU) == 0U) {
        const bool calibrated =
            sensor->imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
        double sum_squares = 0.0;
        for (size_t axis = 0U; axis < 3U; ++axis) {
            const double rate_mdps = (double)sensor->gyro_mdps[axis] -
                (calibrated ? (double)sensor->gyro_bias_mdps[axis] : 0.0);
            sum_squares += rate_mdps * rate_mdps;
        }
        values.gyro_valid = true;
        values.gyro_rate_dps = (float)(sqrt(sum_squares) / 1000.0);
    }
    return values;
}
