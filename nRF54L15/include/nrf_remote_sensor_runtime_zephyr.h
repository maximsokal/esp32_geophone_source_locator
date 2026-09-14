#ifndef NRF_REMOTE_SENSOR_RUNTIME_ZEPHYR_H
#define NRF_REMOTE_SENSOR_RUNTIME_ZEPHYR_H

#include "nrf_bme280.h"
#include "nrf_bmi160.h"
#include "nrf_remote_sensor_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nrf_remote_sensor_runtime_t core;
    nrf_bmi160_t bmi160;
    nrf_bme280_t bme280;
} nrf_remote_sensor_runtime_zephyr_t;

void nrf_remote_sensor_runtime_zephyr_init(
    nrf_remote_sensor_runtime_zephyr_t *runtime);

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime);

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
