#ifndef NRF_IMU_CALIBRATION_H
#define NRF_IMU_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_orientation.h"
#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_IMU_CALIBRATION_WINDOW_MS 1000U
#define NRF_IMU_CALIBRATION_MIN_SAMPLES 8U
#define NRF_IMU_CALIBRATION_GYRO_STDDEV_MAX_MDPS 1500U
/* A capture needs ~1.1 s of samples; one that has not finished after this long is not going to. */
#define NRF_IMU_CALIBRATION_TIMEOUT_MS 5000U

typedef struct {
    int16_t gyro_bias_mdps[3];
    int16_t gravity_ref_mg[3];
    nrf_orientation_reference_axis_t reference_axis;
    nrf_upstream_imu_calibration_state_t state;
    uint16_t generation;
    uint16_t capture_generation;
    uint64_t capture_started_ms;
    uint32_t sample_count;
    int64_t accel_sum[3];
    int64_t gyro_sum[3];
    double gyro_mean[3];
    double gyro_m2[3];
    bool capturing;
} nrf_imu_calibration_t;

void nrf_imu_calibration_init(nrf_imu_calibration_t *calibration);

/*
 * Ends a capture that has been running for NRF_IMU_CALIBRATION_TIMEOUT_MS without finishing (the samples
 * that drive it stopped arriving). Result: SENSOR_INVALID under the capture's own generation, so the ESP
 * can attribute it. Returns true when it just ended one; false when no capture ran or it is still young.
 */
bool nrf_imu_calibration_expire(
    nrf_imu_calibration_t *calibration,
    uint64_t now_uptime_ms);

bool nrf_imu_calibration_begin_capture(
    nrf_imu_calibration_t *calibration,
    uint16_t generation,
    uint64_t start_uptime_ms);

bool nrf_imu_calibration_update(
    nrf_imu_calibration_t *calibration,
    const nrf_imu_sample_t *sample,
    bool sample_valid,
    bool moving,
    uint64_t sample_uptime_ms);

void nrf_imu_calibration_clear(
    nrf_imu_calibration_t *calibration,
    uint16_t generation);

void nrf_imu_calibration_correct_gyro_mdps(
    const nrf_imu_calibration_t *calibration,
    const int32_t gyro_mdps[3],
    int32_t corrected_mdps[3]);

#ifdef __cplusplus
}
#endif

#endif
