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


typedef enum
{
    DEVICE_POWER_SOURCE_UNKNOWN = 0,
    DEVICE_POWER_SOURCE_BATTERY,
    DEVICE_POWER_SOURCE_USB

} device_power_source_t;


/* MAX-245: software over-current state of one INA3221 channel. */
typedef enum
{
    DEVICE_POWER_OVERCURRENT_NONE = 0,
    DEVICE_POWER_OVERCURRENT_WARNING,
    DEVICE_POWER_OVERCURRENT_CRITICAL,

} device_power_overcurrent_t;


typedef struct
{
    bool valid;

    float voltage_v;
    float current_a;
    float power_w;

    device_power_overcurrent_t overcurrent;
    /* A confirmed CRITICAL stays latched until explicitly cleared. */
    bool overcurrent_latched;

} device_power_channel_status_t;


/* MAX-180: user-facing power state (values mirror ina3221_power_ux). */
typedef enum
{
    DEVICE_POWER_STATE_UNKNOWN = 0,
    DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY,
    DEVICE_POWER_STATE_USB_CHARGING,
    DEVICE_POWER_STATE_USB_BATTERY_READY,
    DEVICE_POWER_STATE_BATTERY,
    DEVICE_POWER_STATE_BATTERY_LOW,
    DEVICE_POWER_STATE_BATTERY_CRITICAL,

} device_power_state_t;


/* MAX-183: low-battery reaction (values mirror ina3221_low_battery). */
typedef enum
{
    DEVICE_LOW_BATTERY_NORMAL = 0,
    DEVICE_LOW_BATTERY_WARNING,
    DEVICE_LOW_BATTERY_GRACE,
    DEVICE_LOW_BATTERY_SHUTDOWN,

} device_low_battery_phase_t;


/* MAX-181: where battery_percent_estimate comes from (mirrors ina3221_soc). */
typedef enum
{
    DEVICE_BATTERY_SOC_INVALID = 0,
    DEVICE_BATTERY_SOC_VOLTAGE_ONLY,
    DEVICE_BATTERY_SOC_COULOMB_TRACKED,
    DEVICE_BATTERY_SOC_FULL_ANCHORED,

} device_battery_soc_quality_t;


typedef struct
{
    bool valid;

    float voltage_v;
    float current_a;
    float power_w;

    device_power_source_t source;

    device_power_state_t state;
    /* USB present, not charging and at/above the "full" voltage. */
    bool battery_full;
    device_low_battery_phase_t low_battery;
    /* Seconds until the low-battery shutdown (GRACE only). */
    uint32_t shutdown_in_s;
    device_battery_soc_quality_t battery_soc_quality;

    bool battery_present;
    bool battery_percent_valid;

    float battery_voltage_v;
    float battery_current_a;
    float battery_percent_estimate;

    bool usb_present;
    bool charging;

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

typedef enum
{
    DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED = 0,
    DEVICE_HEIGHT_REFERENCE_VALID,
    DEVICE_HEIGHT_REFERENCE_REJECTED_MOVING,
    DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID

} device_height_reference_state_t;

typedef struct
{
    device_height_reference_state_t state;
    uint16_t generation;
    int32_t relative_z_mm;

} device_height_reference_status_t;

typedef enum
{
    DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED = 0,
    DEVICE_MAIN_HEIGHT_REFERENCE_VALID,
    DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE,
    DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID

} device_main_height_reference_state_t;

typedef struct
{
    device_main_height_reference_state_t state;
    uint16_t generation;
    int32_t relative_z_mm;

} device_main_height_reference_status_t;

typedef struct
{
    bool bmp280_present;
    bool bmp280_valid;
    bool aht20_present;
    bool aht20_valid;
    uint32_t pressure_pa;
    int32_t temperature_mc;
    uint32_t humidity_mpct;
    device_main_height_reference_status_t height_reference;

} device_main_status_t;


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
    device_height_reference_status_t height_reference;
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

    device_main_status_t main;

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
 * 0 = battery branch
 * 1 = ESP32 5 V rail
 * 2 = AD7606 5 V rail
 */
esp_err_t device_status_set_power_channel(
    size_t channel_index,
    float voltage_v,
    float current_a,
    bool valid
);


/*
 * Publish battery/source inference derived from INA3221 measurements.
 * battery_percent_estimate is a voltage-based 1S Li-ion estimate, not
 * coulomb counting, and must only be used when battery_percent_valid is true.
 */
esp_err_t device_status_set_power_battery(
    float battery_voltage_v,
    float battery_current_a,
    float battery_percent_estimate,
    bool battery_present,
    bool battery_percent_valid,
    bool usb_present,
    bool charging,
    device_power_source_t source
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

/* Publish MAIN's directly connected BMP280/AHT20 diagnostics and reference. */
esp_err_t device_status_set_main(const device_main_status_t *main_status);


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


/* Publish the REMOTE's calibrated relative height-reference state. */
esp_err_t device_status_set_pod_height_reference(
    size_t pod_index,
    device_height_reference_state_t state,
    uint16_t generation,
    int32_t relative_z_mm
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
 * Power source name suitable for logs and JSON.
 */
esp_err_t device_status_set_power_channel_overcurrent(
    size_t channel_index,
    device_power_overcurrent_t overcurrent,
    bool latched
);

esp_err_t device_status_set_power_state(
    device_power_state_t state,
    bool battery_full,
    device_low_battery_phase_t low_battery,
    uint32_t shutdown_in_s
);

esp_err_t device_status_set_battery_soc_quality(
    device_battery_soc_quality_t quality
);

const char *device_status_battery_soc_quality_name(
    device_battery_soc_quality_t quality
);

const char *device_status_power_state_name(
    device_power_state_t state
);

const char *device_status_low_battery_name(
    device_low_battery_phase_t phase
);

const char *device_status_power_overcurrent_name(
    device_power_overcurrent_t overcurrent
);

const char *device_status_power_source_name(
    device_power_source_t source
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
