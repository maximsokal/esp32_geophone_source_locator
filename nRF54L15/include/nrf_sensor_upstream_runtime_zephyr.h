#ifndef NRF_SENSOR_UPSTREAM_RUNTIME_ZEPHYR_H
#define NRF_SENSOR_UPSTREAM_RUNTIME_ZEPHYR_H

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif

#endif
