#include "app_pod_status_barometer_policy.h"

app_pod_status_barometer_reading_t app_pod_status_bridge_compute_barometer_reading(
    const nrf_remote_sensor_cache_t *cache)
{
    const app_pod_status_barometer_reading_t invalid = {0.0f, 0.0f, false};

    if (cache == NULL || !cache->response_valid || !cache->topology_current) {
        return invalid;
    }

    if ((cache->sensor.valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) == 0U) {
        return invalid;
    }

    /* altitude_mm is already the fixed-point linear approximation against
     * the standard 101325 Pa reference, computed once on the nRF by
     * nrf_bme280_pressure_to_altitude_mm() (MAX-114/117); this only converts
     * units (mm -> cm), it does not recompute the formula. */
    const app_pod_status_barometer_reading_t reading = {
        (float)cache->sensor.pressure_pa,
        (float)cache->sensor.altitude_mm / 10.0f,
        true,
    };
    return reading;
}
