#ifndef NRF_SENSOR_UPSTREAM_RUNTIME_ZEPHYR_H
#define NRF_SENSOR_UPSTREAM_RUNTIME_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_logger_led.h"
#include "nrf_orientation.h"
#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int nrf_sensor_upstream_runtime_zephyr_configure_orientation(
    const nrf_orientation_config_t *config);

int nrf_sensor_upstream_runtime_zephyr_start(void);

int nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(
    const nrf_upstream_assign_t *assign);

int nrf_sensor_upstream_runtime_zephyr_request_refresh(
    const nrf_upstream_sensor_refresh_t *request);

int nrf_sensor_upstream_runtime_zephyr_request_height_reference(
    const nrf_upstream_height_reference_t *request);
int nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(
    const nrf_upstream_imu_calibrate_t *request);

bool nrf_sensor_upstream_runtime_zephyr_get_snapshot(
    nrf_upstream_sensor_snapshot_t *snapshot);

bool nrf_sensor_upstream_runtime_zephyr_get_orientation(
    nrf_orientation_output_t *orientation);

typedef struct {
    uint32_t activation_generation;
    uint32_t activation_complete_generation;
    bool imu_ready;
    int32_t imu_init_result;
    bool env_ready;
    int32_t env_init_result;
    nrf_logger_led_output_t logger_led_output;
    uint32_t logger_policy_invalid_count;
    uint32_t logger_policy_timestamp_anomaly_count;
    uint8_t orientation_state;
    uint8_t verticality_state;
} nrf_sensor_upstream_runtime_zephyr_diagnostics_t;

/* Lock-free best-effort copy of the sensor worker's last published status. */
bool nrf_sensor_upstream_runtime_zephyr_get_diagnostics(
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t *diagnostics);

#ifdef __cplusplus
}
#endif

#endif
