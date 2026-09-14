#ifndef NRF_ORIENTATION_H
#define NRF_ORIENTATION_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_sensor_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_ORIENTATION_DEFAULT_SAMPLE_PERIOD_MS 100U
#define NRF_ORIENTATION_DEFAULT_GYRO_MOTION_MDPS 15000U
#define NRF_ORIENTATION_DEFAULT_GRAVITY_MIN_MG 750U
#define NRF_ORIENTATION_DEFAULT_GRAVITY_MAX_MG 1250U
#define NRF_ORIENTATION_DEFAULT_EMA_SHIFT 2U

/* sin^2(theta) * 1,000,000 fixed-point thresholds. */
#define NRF_ORIENTATION_DEFAULT_ACCEPT_SIN2_PPM 30154U   /* 10 degrees */
#define NRF_ORIENTATION_DEFAULT_REENTER_SIN2_PPM 19369U  /* 8 degrees */
#define NRF_ORIENTATION_DEFAULT_MEDIUM_SIN2_PPM 116978U  /* 20 degrees */
#define NRF_ORIENTATION_DEFAULT_FAST_SIN2_PPM 328990U    /* 35 degrees */

typedef enum {
    NRF_ORIENTATION_AXIS_UNCONFIGURED = 0,
    NRF_ORIENTATION_AXIS_POS_X,
    NRF_ORIENTATION_AXIS_NEG_X,
    NRF_ORIENTATION_AXIS_POS_Y,
    NRF_ORIENTATION_AXIS_NEG_Y,
    NRF_ORIENTATION_AXIS_POS_Z,
    NRF_ORIENTATION_AXIS_NEG_Z,
} nrf_orientation_reference_axis_t;

typedef enum {
    NRF_ORIENTATION_STATE_UNAVAILABLE = 0,
    NRF_ORIENTATION_STATE_MOVING,
    NRF_ORIENTATION_STATE_ACCEPTED,
    NRF_ORIENTATION_STATE_OUTSIDE,
} nrf_orientation_state_t;

typedef enum {
    NRF_ORIENTATION_REASON_NONE = 0,
    NRF_ORIENTATION_REASON_UNCONFIGURED,
    NRF_ORIENTATION_REASON_SAMPLE_INVALID,
    NRF_ORIENTATION_REASON_GRAVITY_RANGE,
    NRF_ORIENTATION_REASON_MOTION,
} nrf_orientation_reason_t;

typedef enum {
    NRF_ORIENTATION_BLINK_NONE = 0,
    NRF_ORIENTATION_BLINK_SLOW = 1,
    NRF_ORIENTATION_BLINK_MEDIUM = 2,
    NRF_ORIENTATION_BLINK_FAST = 3,
} nrf_orientation_blink_class_t;

typedef struct {
    nrf_orientation_reference_axis_t reference_axis;
    uint32_t sample_period_ms;
    uint32_t gyro_motion_threshold_mdps;
    uint32_t gravity_min_mg;
    uint32_t gravity_max_mg;
    uint32_t accept_sin2_ppm;
    uint32_t reenter_sin2_ppm;
    uint32_t medium_sin2_ppm;
    uint32_t fast_sin2_ppm;
    uint8_t ema_shift;
} nrf_orientation_config_t;

typedef struct {
    bool sample_valid;
    nrf_orientation_state_t state;
    nrf_orientation_reason_t reason;
    nrf_orientation_blink_class_t blink_class;
    uint32_t tilt_sin2_ppm;
    uint32_t gravity_sq_mg2;
    uint32_t gyro_peak_mdps;
    int32_t filtered_accel_mg[3];
} nrf_orientation_output_t;

typedef struct {
    nrf_orientation_config_t config;
    bool filter_initialized;
    bool accepted_latched;
    int32_t filtered_accel_mg[3];
    nrf_orientation_output_t output;
} nrf_orientation_estimator_t;

void nrf_orientation_default_config(nrf_orientation_config_t *config);

bool nrf_orientation_config_valid(const nrf_orientation_config_t *config);

bool nrf_orientation_estimator_init(
    nrf_orientation_estimator_t *estimator,
    const nrf_orientation_config_t *config);

void nrf_orientation_estimator_invalidate(
    nrf_orientation_estimator_t *estimator,
    nrf_orientation_reason_t reason);

bool nrf_orientation_estimator_update(
    nrf_orientation_estimator_t *estimator,
    const nrf_imu_sample_t *sample,
    bool sample_valid);

void nrf_orientation_estimator_get(
    const nrf_orientation_estimator_t *estimator,
    nrf_orientation_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
