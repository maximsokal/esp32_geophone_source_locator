#ifndef NRF_SENSOR_SNAPSHOT_H
#define NRF_SENSOR_SNAPSHOT_H

#include <stdint.h>

#include "nrf_node_state.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NRF_SENSOR_VALID_IMU = 1U << 0,
    NRF_SENSOR_VALID_ENV = 1U << 1,
};

typedef enum {
    NRF_SENSOR_SNAPSHOT_OK = 0,
    NRF_SENSOR_SNAPSHOT_SKIPPED = 1,
    NRF_SENSOR_SNAPSHOT_PARTIAL = 2,
    NRF_SENSOR_SNAPSHOT_ERR_ARGUMENT = -1,
    NRF_SENSOR_SNAPSHOT_ERR_NOT_READY = -2,
    NRF_SENSOR_SNAPSHOT_ERR_NO_DATA = -3,
} nrf_sensor_snapshot_result_t;

typedef struct {
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_milli_c;
} nrf_imu_sample_t;

/* humidity_milli_percent of a sensor that cannot measure humidity (BMP280): not a reading, not 0 %. */
#define NRF_ENV_HUMIDITY_UNAVAILABLE UINT32_MAX

typedef struct {
    int32_t temperature_milli_c;
    uint32_t pressure_pa;
    /* The same pressure in hundredths of a pascal (0 when the source has only whole pascals): a whole pascal is
     * 83 mm of height, the sensors resolve 0.01 Pa. The altitude is computed from this, not from pressure_pa. */
    uint32_t pressure_x100_pa;
    uint32_t humidity_milli_percent;
} nrf_env_sample_t;

/* An IMU read can be pending without a sensor fault or a reusable sample. */
enum { NRF_SENSOR_READ_PENDING = 1 };

typedef int (*nrf_read_imu_fn)(void *context, nrf_imu_sample_t *sample);
typedef int (*nrf_read_env_fn)(void *context, nrf_env_sample_t *sample);

typedef struct {
    nrf_read_imu_fn read_imu;
    void *imu_context;
    nrf_read_env_fn read_env;
    void *env_context;
} nrf_sensor_sources_t;

typedef struct {
    uint32_t sequence;
    uint32_t valid_mask;
    uint32_t fault_mask;
    nrf_imu_sample_t imu;
    nrf_env_sample_t env;
} nrf_sensor_snapshot_t;

void nrf_sensor_snapshot_init(nrf_sensor_snapshot_t *snapshot);

nrf_sensor_snapshot_result_t nrf_sensor_snapshot_refresh(
    const nrf_node_state_t *node_state,
    const nrf_sensor_sources_t *sources,
    nrf_sensor_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
