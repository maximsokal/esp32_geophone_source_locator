#include <assert.h>
#include <string.h>

#include "app_pod_status_barometer_policy.h"

static nrf_remote_sensor_cache_t make_cache(
    bool response_valid,
    bool topology_current,
    uint32_t valid_mask,
    uint32_t pressure_pa,
    int32_t altitude_mm)
{
    nrf_remote_sensor_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.response_valid = response_valid;
    cache.topology_current = topology_current;
    cache.sensor.valid_mask = valid_mask;
    cache.sensor.pressure_pa = pressure_pa;
    cache.sensor.altitude_mm = altitude_mm;
    return cache;
}

static void test_null_cache_is_invalid(void)
{
    const app_pod_status_barometer_reading_t reading =
        app_pod_status_bridge_compute_barometer_reading(NULL);
    assert(!reading.valid);
    assert(reading.pressure_pa == 0.0f);
    assert(reading.relative_altitude_cm == 0.0f);
}

static void test_stale_or_unresponded_cache_is_invalid(void)
{
    nrf_remote_sensor_cache_t cache =
        make_cache(false, true, NRF_UPSTREAM_SENSOR_VALID_ENV, 101325U, 0);
    assert(!app_pod_status_bridge_compute_barometer_reading(&cache).valid);

    cache = make_cache(true, false, NRF_UPSTREAM_SENSOR_VALID_ENV, 101325U, 0);
    assert(!app_pod_status_bridge_compute_barometer_reading(&cache).valid);
}

static void test_env_domain_invalid_is_invalid(void)
{
    /* Fresh, current response, but the ENV bit (pressure/altitude) is not
     * set -- e.g. only the IMU domain answered this cycle. */
    const nrf_remote_sensor_cache_t cache =
        make_cache(true, true, NRF_UPSTREAM_SENSOR_VALID_IMU, 101325U, 0);
    assert(!app_pod_status_bridge_compute_barometer_reading(&cache).valid);
}

static void test_fresh_valid_reading_converts_units(void)
{
    const nrf_remote_sensor_cache_t cache =
        make_cache(true, true, NRF_UPSTREAM_SENSOR_VALID_ENV, 100000U, 10000);
    const app_pod_status_barometer_reading_t reading =
        app_pod_status_bridge_compute_barometer_reading(&cache);
    assert(reading.valid);
    assert(reading.pressure_pa == 100000.0f);
    /* altitude_mm is already computed on the nRF; this only converts to cm. */
    assert(reading.relative_altitude_cm == 1000.0f);
}

int main(void)
{
    test_null_cache_is_invalid();
    test_stale_or_unresponded_cache_is_invalid();
    test_env_domain_invalid_is_invalid();
    test_fresh_valid_reading_converts_units();
    return 0;
}
