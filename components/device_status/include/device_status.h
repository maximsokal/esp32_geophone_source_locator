#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define DEVICE_STATUS_POD_COUNT            4
#define DEVICE_STATUS_POWER_CHANNEL_COUNT  3
#define DEVICE_STATUS_MUX_CHANNEL_COUNT    8


typedef enum
{
    DEVICE_MODE_IDLE = 0,
    DEVICE_MODE_MEASUREMENT,
    DEVICE_MODE_WIFI

} device_mode_t;


typedef enum
{
    POD_STATE_DISCONNECTED = 0,
    POD_STATE_READY,
    POD_STATE_WARNING,
    POD_STATE_ERROR

} pod_state_t;


typedef struct
{
    bool valid;

    float voltage_v;
    float current_a;
    float power_w;

} device_power_channel_status_t;


typedef struct
{
    bool valid;

    float voltage_v;
    float current_a;
    float power_w;

    device_power_channel_status_t
        channels[DEVICE_STATUS_POWER_CHANNEL_COUNT];

} device_power_status_t;


typedef struct
{
    bool valid;

    float temperature_c;
    float humidity_percent;
    float pressure_pa;

} device_environment_status_t;


typedef struct
{
    bool valid;

    int active_channel;
    int active_pod;

} device_mux_status_t;


typedef struct
{
    bool valid;

    float pressure_pa;
    float relative_altitude_cm;
    float reference_pressure_pa;

} device_barometer_status_t;


typedef struct
{
    bool valid;

    float roll_deg;
    float pitch_deg;
    float yaw_relative_deg;

} device_imu_status_t;


typedef struct
{
    bool valid;
    bool temperature_valid;
    bool humidity_valid;

    float temperature_c;
    float humidity_percent;

} device_pod_environment_status_t;


typedef struct
{
    bool local_valid;
    float local_x_m;
    float local_y_m;

    bool global_valid;
    double latitude;
    double longitude;

    bool altitude_valid;
    double altitude_m;

} device_pod_position_status_t;


typedef struct
{
    bool barometer;
    bool environment;
    bool imu;
    bool geophone;
    bool local_position;
    bool global_position;

} device_pod_capabilities_t;


typedef struct
{
    bool valid;

    float instant_uv;
    float rms_uv;
    float peak_uv;

    float noise_rms_uv;
    float snr_db;

} device_geophone_status_t;


typedef struct
{
    pod_state_t state;

    device_pod_capabilities_t capabilities;
    device_barometer_status_t barometer;
    device_pod_environment_status_t environment;
    device_imu_status_t imu;
    device_geophone_status_t geophone;
    device_pod_position_status_t position;

} device_pod_status_t;


typedef struct
{
    bool running;

    uint32_t sample_rate_hz;

    uint64_t samples_recorded;
    uint64_t recording_ms;

} device_acquisition_status_t;


typedef struct
{
    device_mode_t mode;

    device_power_status_t power;

    device_environment_status_t environment;

    device_mux_status_t mux;

    device_pod_status_t
        pods[DEVICE_STATUS_POD_COUNT];

    device_acquisition_status_t acquisition;

} device_status_snapshot_t;


/*
 * Initialize the shared device status model.
 */
esp_err_t device_status_init(void);


/*
 * Copy the complete current device status.
 */
esp_err_t device_status_get_snapshot(
    device_status_snapshot_t *snapshot
);


/*
 * Set the primary power values shown by the UI.
 */
esp_err_t device_status_set_power(
    float voltage_v,
    float current_a,
    bool valid
);


/*
 * Update one INA3221 measurement channel.
 *
 * channel_index is zero-based:
 * 0, 1 or 2.
 */
esp_err_t device_status_set_power_channel(
    size_t channel_index,
    float voltage_v,
    float current_a,
    bool valid
);


/*
 * Update enclosure environmental measurements.
 */
esp_err_t device_status_set_environment(
    float temperature_c,
    float humidity_percent,
    float pressure_pa,
    bool valid
);


/*
 * Update the active TCA9548A channel and pod.
 *
 * active_channel:
 * -1 means no active channel
 * 0..7 are TCA9548A channels.
 *
 * active_pod:
 * -1 means no active pod
 * 0..3 are logical pods.
 */
esp_err_t device_status_set_mux(
    int active_channel,
    int active_pod,
    bool valid
);


/*
 * Set the high-level operating mode.
 */
esp_err_t device_status_set_mode(
    device_mode_t mode
);


/*
 * Set the connection/health state of one sensor pod.
 */
esp_err_t device_status_set_pod_state(
    size_t pod_index,
    pod_state_t state
);


/*
 * Update one pod barometer measurement.
 */
esp_err_t device_status_set_pod_barometer(
    size_t pod_index,
    float pressure_pa,
    float relative_altitude_cm,
    float reference_pressure_pa,
    bool valid
);


/*
 * Update one pod BMI160 orientation.
 *
 * yaw_relative_deg is relative only.
 * BMI160 has no magnetometer.
 */
esp_err_t device_status_set_pod_imu(
    size_t pod_index,
    float roll_deg,
    float pitch_deg,
    float yaw_relative_deg,
    bool valid
);


/*
 * Update temperature/humidity values published by the pod environmental
 * sensor. Capability becomes known as soon as a driver calls this function;
 * individual validity remains false until that value is actually measured.
 */
esp_err_t device_status_set_pod_environment(
    size_t pod_index,
    float temperature_c,
    bool temperature_valid,
    float humidity_percent,
    bool humidity_valid
);


/*
 * Publish a local pod position from a real positioning transport. Calling
 * this with valid=false advertises transport capability without inventing a
 * coordinate.
 */
esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
);


/*
 * Publish resolved global coordinates. Until a real transport/localization
 * layer calls this setter, global_position capability and validity stay false.
 */
esp_err_t device_status_set_pod_global_position(
    size_t pod_index,
    double latitude,
    double longitude,
    double altitude_m,
    bool altitude_valid,
    bool valid
);


/*
 * Update one geophone signal summary.
 *
 * Complete waveform history is intentionally not
 * stored in device_status.
 */
esp_err_t device_status_set_pod_geophone(
    size_t pod_index,
    float instant_uv,
    float rms_uv,
    float peak_uv,
    float noise_rms_uv,
    float snr_db,
    bool valid
);


/*
 * Update acquisition and recording status.
 */
esp_err_t device_status_set_acquisition(
    bool running,
    uint32_t sample_rate_hz,
    uint64_t samples_recorded,
    uint64_t recording_ms
);


/*
 * Short mode name suitable for the TFT status bar
 * and JSON API.
 */
const char *device_status_mode_name(
    device_mode_t mode
);


/*
 * Full pod state name suitable for JSON.
 */
const char *device_status_pod_name(
    pod_state_t state
);


/*
 * Short pod state suitable for the TFT status bar.
 */
const char *device_status_pod_short_name(
    pod_state_t state
);
