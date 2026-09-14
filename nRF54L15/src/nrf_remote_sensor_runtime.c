#include "nrf_remote_sensor_runtime.h"

#include <string.h>

static void nrf_remote_sensor_runtime_clear_source(
    nrf_sensor_sources_t *sources,
    bool imu)
{
    if (imu) {
        sources->read_imu = NULL;
        sources->imu_context = NULL;
    } else {
        sources->read_env = NULL;
        sources->env_context = NULL;
    }
}

void nrf_remote_sensor_runtime_init(nrf_remote_sensor_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
    nrf_sensor_snapshot_init(&runtime->snapshot);
}

static void nrf_remote_sensor_prepare_imu(
    const nrf_remote_sensor_ops_t *ops,
    nrf_remote_sensor_runtime_t *runtime)
{
    if (runtime->imu_ready) {
        return;
    }

    nrf_remote_sensor_runtime_clear_source(&runtime->sources, true);
    if (ops->init_imu == NULL || ops->read_imu == NULL) {
        runtime->imu_init_result = NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA;
        return;
    }

    runtime->imu_init_result = ops->init_imu(ops->imu_context);
    if (runtime->imu_init_result == 0) {
        runtime->sources.read_imu = ops->read_imu;
        runtime->sources.imu_context = ops->imu_context;
        runtime->imu_ready = true;
    }
}

static void nrf_remote_sensor_prepare_env(
    const nrf_remote_sensor_ops_t *ops,
    nrf_remote_sensor_runtime_t *runtime)
{
    if (runtime->env_ready) {
        return;
    }

    nrf_remote_sensor_runtime_clear_source(&runtime->sources, false);
    if (ops->init_env == NULL || ops->read_env == NULL) {
        runtime->env_init_result = NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA;
        return;
    }

    runtime->env_init_result = ops->init_env(ops->env_context);
    if (runtime->env_init_result == 0) {
        runtime->sources.read_env = ops->read_env;
        runtime->sources.env_context = ops->env_context;
        runtime->env_ready = true;
    }
}

nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_prepare(
    const nrf_node_state_t *node_state,
    const nrf_remote_sensor_ops_t *ops,
    nrf_remote_sensor_runtime_t *runtime)
{
    if (node_state == NULL || ops == NULL || runtime == NULL) {
        return NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT;
    }

    if (node_state->lifecycle != NRF_NODE_LIFECYCLE_READY) {
        return NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY;
    }

    if (node_state->role != NRF_NODE_ROLE_REMOTE) {
        return NRF_REMOTE_SENSOR_PREPARE_SKIPPED;
    }

    nrf_remote_sensor_prepare_imu(ops, runtime);
    nrf_remote_sensor_prepare_env(ops, runtime);

    if (runtime->imu_ready && runtime->env_ready) {
        return NRF_REMOTE_SENSOR_PREPARE_OK;
    }
    if (runtime->imu_ready || runtime->env_ready) {
        return NRF_REMOTE_SENSOR_PREPARE_PARTIAL;
    }
    return NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA;
}

nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_t *runtime)
{
    if (node_state == NULL || runtime == NULL) {
        return NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT;
    }

    return nrf_sensor_snapshot_refresh(
        node_state,
        &runtime->sources,
        &runtime->snapshot);
}
