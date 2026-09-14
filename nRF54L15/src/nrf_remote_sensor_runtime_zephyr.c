#include "nrf_remote_sensor_runtime_zephyr.h"

#include <string.h>

#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"

static int nrf_remote_sensor_zephyr_init_imu(void *context)
{
    nrf_bmi160_t *device = (nrf_bmi160_t *)context;
    return (int)nrf_bmi160_zephyr_init_remote(device);
}

static int nrf_remote_sensor_zephyr_init_env(void *context)
{
    nrf_bme280_t *device = (nrf_bme280_t *)context;
    return (int)nrf_bme280_zephyr_init_remote(device);
}

void nrf_remote_sensor_runtime_zephyr_init(
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    nrf_remote_sensor_runtime_init(&runtime->core);
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_zephyr_prepare(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (node_state == NULL || runtime == NULL) {
        return NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT;
    }

    const nrf_remote_sensor_ops_t ops = {
        .init_imu = nrf_remote_sensor_zephyr_init_imu,
        .read_imu = nrf_bmi160_read_imu,
        .imu_context = &runtime->bmi160,
        .init_env = nrf_remote_sensor_zephyr_init_env,
        .read_env = nrf_bme280_read_env,
        .env_context = &runtime->bme280,
    };

    return nrf_remote_sensor_runtime_prepare(
        node_state,
        &ops,
        &runtime->core);
}

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_zephyr_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_zephyr_t *runtime)
{
    if (runtime == NULL) {
        return NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT;
    }

    return nrf_remote_sensor_runtime_refresh(node_state, &runtime->core);
}
