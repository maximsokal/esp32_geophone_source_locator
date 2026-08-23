#include "device_status.h"

#include "device_status_internal.h"


esp_err_t device_status_set_power(
    float voltage_v,
    float current_a,
    bool valid
)
{
    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    status->power.valid =
        valid;

    status->power.voltage_v =
        voltage_v;

    status->power.current_a =
        current_a;

    status->power.power_w =
        voltage_v * current_a;


    device_status_internal_unlock();


    return ESP_OK;
}


esp_err_t device_status_set_power_channel(
    size_t channel_index,
    float voltage_v,
    float current_a,
    bool valid
)
{
    if (
        channel_index >=
        DEVICE_STATUS_POWER_CHANNEL_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    device_power_channel_status_t *channel =
        &status->power.channels[
            channel_index
        ];


    channel->valid =
        valid;

    channel->voltage_v =
        voltage_v;

    channel->current_a =
        current_a;

    channel->power_w =
        voltage_v * current_a;


    device_status_internal_unlock();


    return ESP_OK;
}


esp_err_t device_status_set_environment(
    float temperature_c,
    float humidity_percent,
    float pressure_pa,
    bool valid
)
{
    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    status->environment.valid =
        valid;

    status->environment.temperature_c =
        temperature_c;

    status->environment.humidity_percent =
        humidity_percent;

    status->environment.pressure_pa =
        pressure_pa;


    device_status_internal_unlock();


    return ESP_OK;
}


esp_err_t device_status_set_mux(
    int active_channel,
    int active_pod,
    bool valid
)
{
    if (
        active_channel < -1 ||
        active_channel >=
            DEVICE_STATUS_MUX_CHANNEL_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        active_pod < -1 ||
        active_pod >=
            DEVICE_STATUS_POD_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    status->mux.valid =
        valid;

    status->mux.active_channel =
        active_channel;

    status->mux.active_pod =
        active_pod;


    device_status_internal_unlock();


    return ESP_OK;
}


esp_err_t device_status_set_mode(
    device_mode_t mode
)
{
    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    status->mode =
        mode;


    device_status_internal_unlock();


    return ESP_OK;
}


esp_err_t device_status_set_acquisition(
    bool running,
    uint32_t sample_rate_hz,
    uint64_t samples_recorded,
    uint64_t recording_ms
)
{
    device_status_snapshot_t *status;


    esp_err_t err =
        device_status_internal_lock(
            &status
        );


    if (err != ESP_OK) {
        return err;
    }


    status->acquisition.running =
        running;

    status->acquisition.sample_rate_hz =
        sample_rate_hz;

    status->acquisition.samples_recorded =
        samples_recorded;

    status->acquisition.recording_ms =
        recording_ms;


    device_status_internal_unlock();


    return ESP_OK;
}