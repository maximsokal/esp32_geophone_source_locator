#include "nrf_sensor_snapshot.h"

#include <string.h>

static void nrf_sensor_snapshot_clear_payload(nrf_sensor_snapshot_t *snapshot)
{
    snapshot->valid_mask = 0U;
    snapshot->fault_mask = 0U;
    memset(&snapshot->imu, 0, sizeof(snapshot->imu));
    memset(&snapshot->env, 0, sizeof(snapshot->env));
}

void nrf_sensor_snapshot_init(nrf_sensor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
}

nrf_sensor_snapshot_result_t nrf_sensor_snapshot_refresh(
    const nrf_node_state_t *node_state,
    const nrf_sensor_sources_t *sources,
    nrf_sensor_snapshot_t *snapshot)
{
    if (node_state == NULL || sources == NULL || snapshot == NULL) {
        return NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT;
    }

    nrf_sensor_snapshot_clear_payload(snapshot);

    if (node_state->lifecycle != NRF_NODE_LIFECYCLE_READY) {
        return NRF_SENSOR_SNAPSHOT_ERR_NOT_READY;
    }

    if (node_state->role != NRF_NODE_ROLE_REMOTE) {
        return NRF_SENSOR_SNAPSHOT_SKIPPED;
    }

    snapshot->sequence++;

    if (sources->read_imu == NULL) {
        snapshot->fault_mask |= NRF_SENSOR_VALID_IMU;
    } else {
        nrf_imu_sample_t imu = {0};
        const int result = sources->read_imu(sources->imu_context, &imu);
        if (result == 0) {
            snapshot->imu = imu;
            snapshot->valid_mask |= NRF_SENSOR_VALID_IMU;
        } else if (result != NRF_SENSOR_READ_PENDING) {
            snapshot->fault_mask |= NRF_SENSOR_VALID_IMU;
        }
    }

    if (sources->read_env == NULL) {
        snapshot->fault_mask |= NRF_SENSOR_VALID_ENV;
    } else {
        nrf_env_sample_t env = {0};
        if (sources->read_env(sources->env_context, &env) == 0) {
            snapshot->env = env;
            snapshot->valid_mask |= NRF_SENSOR_VALID_ENV;
        } else {
            snapshot->fault_mask |= NRF_SENSOR_VALID_ENV;
        }
    }

    const uint32_t expected_mask = NRF_SENSOR_VALID_IMU | NRF_SENSOR_VALID_ENV;
    if (snapshot->valid_mask == expected_mask) {
        return NRF_SENSOR_SNAPSHOT_OK;
    }
    if (snapshot->valid_mask != 0U) {
        return NRF_SENSOR_SNAPSHOT_PARTIAL;
    }
    return NRF_SENSOR_SNAPSHOT_ERR_NO_DATA;
}
