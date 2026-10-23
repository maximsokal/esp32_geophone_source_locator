#include <assert.h>
#include <string.h>
#include "app_pod_status_sensor_health_policy.h"
#include "nrf_node_link_status_policy.h"

int main(void)
{
    nrf_remote_sensor_cache_t cache = {0};
    assert(!app_pod_status_sensor_health(NULL, 100U).current);
    assert(!app_pod_status_sensor_health(&cache, 100U).current);
    cache.response_valid = true;
    cache.topology_current = true;
    cache.identity_valid = true;
    cache.esp_receipt_uptime_ms = 100U;
    cache.sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_IMU | NRF_UPSTREAM_SENSOR_VALID_ENV;
    device_sensor_health_t health = app_pod_status_sensor_health(&cache, 101U);
    assert(health.current && health.imu_valid && health.barometer_valid);
    assert(!health.imu_calibrated);
    cache.sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    assert(app_pod_status_sensor_health(&cache, 101U).imu_calibrated);
    cache.sensor.fault_mask = NRF_UPSTREAM_SENSOR_VALID_IMU;
    health = app_pod_status_sensor_health(&cache, 101U);
    assert(health.imu_fault && !health.imu_valid && !health.imu_calibrated);
    assert(health.barometer_valid && !health.barometer_fault);
    cache.sensor.fault_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
    health = app_pod_status_sensor_health(&cache, 101U);
    assert(health.barometer_fault && !health.barometer_valid && health.imu_valid);
    assert(!app_pod_status_sensor_health(&cache, 100U + NRF_NODE_LINK_STATUS_STALE_LIMIT_MS).current);
    assert(!app_pod_status_sensor_health(&cache, 99U).current);
    cache.topology_current = false;
    assert(!app_pod_status_sensor_health(&cache, 101U).current);
    cache.topology_current = true;
    cache.identity_valid = false;
    assert(!app_pod_status_sensor_health(&cache, 101U).current);
    return 0;
}
