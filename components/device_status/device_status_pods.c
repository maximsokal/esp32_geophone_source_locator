#include "device_status.h"

#include <math.h>

#include "device_status_internal.h"


static esp_err_t device_status_validate_pod(
    size_t pod_index
)
{
    if (
        pod_index >=
        DEVICE_STATUS_POD_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}


esp_err_t device_status_set_pod_state(
    size_t pod_index,
    pod_state_t state
)
{
    esp_err_t err =
        device_status_validate_pod(
            pod_index
        );

    if (err != ESP_OK) {
        return err;
    }

    device_status_snapshot_t *status;

    err =
        device_status_internal_lock(
            &status
        );

    if (err != ESP_OK) {
        return err;
    }

    status->pods[pod_index].state =
        state;

    device_status_internal_unlock();

    return ESP_OK;
}


esp_err_t device_status_set_pod_barometer(
    size_t pod_index,
    float pressure_pa,
    float relative_altitude_cm,
    float reference_pressure_pa,
    bool valid
)
{
    esp_err_t err =
        device_status_validate_pod(
            pod_index
        );

    if (err != ESP_OK) {
        return err;
    }

    device_status_snapshot_t *status;

    err =
        device_status_internal_lock(
            &status
        );

    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod =
        &status->pods[pod_index];

    pod->capabilities.barometer =
        true;

    device_barometer_status_t *barometer =
        &pod->barometer;

    barometer->valid =
        valid;

    barometer->pressure_pa =
        pressure_pa;

    barometer->relative_altitude_cm =
        relative_altitude_cm;

    barometer->reference_pressure_pa =
        reference_pressure_pa;

    device_status_internal_unlock();

    return ESP_OK;
}


esp_err_t device_status_set_pod_height_reference(
    size_t pod_index,
    device_height_reference_state_t state,
    uint16_t generation,
    int32_t relative_z_mm)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }
    if ((unsigned)state > (unsigned)DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID ||
        (state != DEVICE_HEIGHT_REFERENCE_VALID && relative_z_mm != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    status->pods[pod_index].height_reference.state = state;
    status->pods[pod_index].height_reference.generation = generation;
    status->pods[pod_index].height_reference.relative_z_mm = relative_z_mm;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_imu(
    size_t pod_index,
    float roll_deg,
    float pitch_deg,
    float yaw_relative_deg,
    bool valid
)
{
    esp_err_t err =
        device_status_validate_pod(
            pod_index
        );

    if (err != ESP_OK) {
        return err;
    }

    device_status_snapshot_t *status;

    err =
        device_status_internal_lock(
            &status
        );

    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod =
        &status->pods[pod_index];

    pod->capabilities.imu =
        true;

    device_imu_status_t *imu =
        &pod->imu;

    imu->valid =
        valid;

    imu->roll_deg =
        roll_deg;

    imu->pitch_deg =
        pitch_deg;

    imu->yaw_relative_deg =
        yaw_relative_deg;

    device_status_internal_unlock();

    return ESP_OK;
}


esp_err_t device_status_set_pod_environment(
    size_t pod_index,
    float temperature_c,
    bool temperature_valid,
    float humidity_percent,
    bool humidity_valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }

    if (
        (temperature_valid && !isfinite(temperature_c)) ||
        (humidity_valid && (
            !isfinite(humidity_percent) ||
            humidity_percent < 0.0f ||
            humidity_percent > 100.0f
        ))
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.environment = true;
    pod->environment.valid = temperature_valid || humidity_valid;
    pod->environment.temperature_valid = temperature_valid;
    pod->environment.humidity_valid = humidity_valid;
    pod->environment.temperature_c = temperature_valid ? temperature_c : 0.0f;
    pod->environment.humidity_percent = humidity_valid ? humidity_percent : 0.0f;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }

    if (valid && (!isfinite(local_x_m) || !isfinite(local_y_m))) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.local_position = true;
    pod->position.local_valid = valid;
    pod->position.local_x_m = valid ? local_x_m : 0.0f;
    pod->position.local_y_m = valid ? local_y_m : 0.0f;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_global_position(
    size_t pod_index,
    double latitude,
    double longitude,
    double altitude_m,
    bool altitude_valid,
    bool valid
)
{
    esp_err_t err = device_status_validate_pod(pod_index);
    if (err != ESP_OK) {
        return err;
    }

    if (
        valid && (
            !isfinite(latitude) ||
            !isfinite(longitude) ||
            latitude < -90.0 ||
            latitude > 90.0 ||
            longitude < -180.0 ||
            longitude > 180.0 ||
            (altitude_valid && !isfinite(altitude_m))
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    device_status_snapshot_t *status;
    err = device_status_internal_lock(&status);
    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod = &status->pods[pod_index];
    pod->capabilities.global_position = true;
    pod->position.global_valid = valid;
    pod->position.latitude = valid ? latitude : 0.0;
    pod->position.longitude = valid ? longitude : 0.0;
    pod->position.altitude_valid = valid && altitude_valid;
    pod->position.altitude_m = valid && altitude_valid ? altitude_m : 0.0;

    device_status_internal_unlock();
    return ESP_OK;
}


esp_err_t device_status_set_pod_geophone(
    size_t pod_index,
    float instant_uv,
    float rms_uv,
    float peak_uv,
    float noise_rms_uv,
    float snr_db,
    bool valid
)
{
    esp_err_t err =
        device_status_validate_pod(
            pod_index
        );

    if (err != ESP_OK) {
        return err;
    }

    device_status_snapshot_t *status;

    err =
        device_status_internal_lock(
            &status
        );

    if (err != ESP_OK) {
        return err;
    }

    device_pod_status_t *pod =
        &status->pods[pod_index];

    pod->capabilities.geophone =
        true;

    device_geophone_status_t *geophone =
        &pod->geophone;

    geophone->valid =
        valid;

    geophone->instant_uv =
        instant_uv;

    geophone->rms_uv =
        rms_uv;

    geophone->peak_uv =
        peak_uv;

    geophone->noise_rms_uv =
        noise_rms_uv;

    geophone->snr_db =
        snr_db;

    device_status_internal_unlock();

    return ESP_OK;
}
