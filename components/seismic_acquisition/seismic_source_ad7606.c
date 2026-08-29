#include "seismic_source_driver.h"

#include <stddef.h>

#include "sdkconfig.h"

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "app_task_config.h"


static esp_err_t seismic_source_ad7606_snapshot(
    seismic_acquisition_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ad7606_acquisition_status_t status;

    esp_err_t err = ad7606_acquisition_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }

    snapshot->running = status.running;
    snapshot->latest_valid = status.latest_valid;
    snapshot->samples = status.samples;
    snapshot->read_errors = status.read_errors;
    snapshot->consecutive_failures = status.consecutive_failures;
    snapshot->last_sample_timestamp_us = status.last_sample_timestamp_us;

    snapshot->bus_clock_hz = AD7606_BUS_CLOCK_HZ;
    snapshot->bus_mode = AD7606_SPI_MODE;
    snapshot->input_range_v = AD7606_INPUT_RANGE_V;
    snapshot->oversampling_ratio = AD7606_OVERSAMPLING_RATIO;

    for (
        size_t channel = 0;
        channel < AD7606_CHANNEL_COUNT;
        ++channel
    ) {
        snapshot->latest_uv[channel] = status.latest_uv[channel];
    }

    return ESP_OK;
}


const seismic_source_driver_t g_seismic_source_ad7606 = {
    .name = "AD7606",
    .transport_name = "SPI3",
    .sample_rate_hz = CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
    .channel_count = AD7606_CHANNEL_COUNT,
    .core_id = APP_TASK_CORE_REALTIME,
    .start = ad7606_acquisition_start,
    .stop = ad7606_acquisition_stop,
    .populate_snapshot = seismic_source_ad7606_snapshot,
};
