#ifndef NRF_REMOTE_SENSOR_RUNTIME_H
#define NRF_REMOTE_SENSOR_RUNTIME_H

#include <stdbool.h>

#include "nrf_node_state.h"
#include "nrf_sensor_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_REMOTE_SENSOR_PREPARE_OK = 0,
    NRF_REMOTE_SENSOR_PREPARE_SKIPPED = 1,
    NRF_REMOTE_SENSOR_PREPARE_PARTIAL = 2,
    NRF_REMOTE_SENSOR_PREPARE_ERR_ARGUMENT = -1,
    NRF_REMOTE_SENSOR_PREPARE_ERR_NOT_READY = -2,
    NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA = -3,
} nrf_remote_sensor_prepare_result_t;

typedef int (*nrf_remote_sensor_init_fn)(void *context);

typedef struct {
    nrf_remote_sensor_init_fn init_imu;
    nrf_read_imu_fn read_imu;
    void *imu_context;
    nrf_remote_sensor_init_fn init_env;
    nrf_read_env_fn read_env;
    void *env_context;
} nrf_remote_sensor_ops_t;

typedef struct {
    nrf_sensor_sources_t sources;
    nrf_sensor_snapshot_t snapshot;
    int imu_init_result;
    int env_init_result;
    bool imu_ready;
    bool env_ready;
} nrf_remote_sensor_runtime_t;

void nrf_remote_sensor_runtime_init(nrf_remote_sensor_runtime_t *runtime);

/*
 * Prepare optional REMOTE-only sensors after runtime role assignment.
 * MAIN and UNASSIGNED nodes are intentionally skipped without sensor I/O.
 * Successfully initialized sensors are not reinitialized on later calls;
 * failed sensors may be retried explicitly by calling prepare again.
 */
nrf_remote_sensor_prepare_result_t nrf_remote_sensor_runtime_prepare(
    const nrf_node_state_t *node_state,
    const nrf_remote_sensor_ops_t *ops,
    nrf_remote_sensor_runtime_t *runtime);

/*
 * Refresh the aggregate snapshot from only the sources that initialized
 * successfully. nrf_sensor_snapshot_refresh clears payload validity on every
 * attempt, so a failed read cannot leak a stale previous sample.
 */
nrf_sensor_snapshot_result_t nrf_remote_sensor_runtime_refresh(
    const nrf_node_state_t *node_state,
    nrf_remote_sensor_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
