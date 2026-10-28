#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "nrf_upstream_protocol.h"
#include "app_pod_status_sensor_values_policy.h"

int main(void)
{
    nrf_upstream_status_t status = {0}, decoded_status = {0};
    nrf_upstream_sensor_snapshot_t sensor = {0};
    nrf_remote_sensor_cache_t cache = {0};
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    status.request_id = 1U;
    status.hardware_id_length = 1U;
    status.hardware_id[0] = 42U;
    status.role = NRF_UPSTREAM_ROLE_REMOTE;
    status.node_id = 1U;
    status.mux_channel = 0U;
    status.target_address = NRF_UPSTREAM_ADDRESS_REMOTE;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    status.flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
    sensor.request_id = 2U;
    sensor.sequence = 1U;
    sensor.result = NRF_UPSTREAM_SENSOR_RESULT_PARTIAL;
    sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
    sensor.fault_mask = NRF_UPSTREAM_SENSOR_VALID_IMU;
    sensor.flags = NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED | NRF_UPSTREAM_SENSOR_FLAG_ENV_READY |
                   NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
    sensor.env_temperature_milli_c = 24250;
    sensor.pressure_pa = 100000U;
    sensor.humidity_milli_percent = UINT32_MAX;
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_OK);
    assert(nrf_upstream_protocol_decode_remote_snapshot(frame, sizeof(frame), &decoded_status, &cache.sensor) == NRF_UPSTREAM_RESULT_OK);
    assert(cache.sensor.humidity_milli_percent == UINT32_MAX);
    cache.response_valid = true;
    cache.topology_current = true;
    const app_pod_status_sensor_values_t values = app_pod_status_bridge_compute_sensor_values(&cache);
    assert(values.temperature_valid && values.temperature_c == 24.25f);
    assert(!values.humidity_valid && values.humidity_percent == 0.0f);
    return 0;
}
