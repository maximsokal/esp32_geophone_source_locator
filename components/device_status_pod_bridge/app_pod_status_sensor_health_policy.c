#include "app_pod_status_sensor_health_policy.h"
#include "nrf_node_link_status_policy.h"

device_sensor_health_t app_pod_status_sensor_health(
    const nrf_remote_sensor_cache_t *cache, uint64_t now_ms)
{
    device_sensor_health_t health = {0};
    if (cache == NULL || !cache->response_valid || !cache->topology_current ||
        !cache->identity_valid || now_ms < cache->esp_receipt_uptime_ms ||
        now_ms - cache->esp_receipt_uptime_ms >= NRF_NODE_LINK_STATUS_STALE_LIMIT_MS) {
        return health;
    }
    health.current = true;
    health.imu_fault = (cache->sensor.fault_mask & NRF_UPSTREAM_SENSOR_VALID_IMU) != 0U;
    health.barometer_fault = (cache->sensor.fault_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U;
    health.imu_valid = !health.imu_fault &&
        (cache->sensor.valid_mask & NRF_UPSTREAM_SENSOR_VALID_IMU) != 0U;
    health.barometer_valid = !health.barometer_fault &&
        (cache->sensor.valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U;
    health.imu_calibrated = health.imu_valid &&
        cache->sensor.imu_cal_state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    return health;
}
