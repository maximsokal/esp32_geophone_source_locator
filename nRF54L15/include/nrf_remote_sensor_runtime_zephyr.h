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
    nrf_env_sample_t prepared_env;
    nrf_bme280_result_t prepared_env_result;
    bool env_prepared;
    uint64_t imu_read_deadline_ms;
    bool imu_init_active;
    bool env_init_active;
    uint8_t imu_address_index;
    int imu_best_result;
} nrf_remote_sensor_runtime_zephyr_t;

void nrf_remote_sensor_runtime_zephyr_init(
    nrf_remote_sensor_runtime_zephyr_t *runtime);
void nrf_remote_sensor_runtime_zephyr_note_imu_read(
    nrf_remote_sensor_runtime_zephyr_t *runtime, bool faulted);

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime);
nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare_cooperative(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime);

/* Begin/poll one fresh environment conversion; refresh consumes its result. */
nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_begin_env(
    const nrf_node_state_t *node_state, nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms);
nrf_bme280_result_t nrf_remote_sensor_runtime_zephyr_poll_env(
    nrf_remote_sensor_runtime_zephyr_t *runtime, uint64_t now_ms);

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
