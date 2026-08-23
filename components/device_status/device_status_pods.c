#include "device_status.h"

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


    device_barometer_status_t *barometer =
        &status->pods[
            pod_index
        ].barometer;


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


    device_imu_status_t *imu =
        &status->pods[
            pod_index
        ].imu;


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


    device_geophone_status_t *geophone =
        &status->pods[
            pod_index
        ].geophone;


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