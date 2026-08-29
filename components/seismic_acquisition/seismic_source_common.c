#include "seismic_source_driver.h"

#include "seismic_buffer.h"


esp_err_t seismic_source_snapshot_from_buffer(
    seismic_acquisition_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_frame_t frame;
    size_t frame_count = 0;
    seismic_buffer_info_t info;

    esp_err_t err = seismic_buffer_copy_latest(
        &frame,
        1,
        &frame_count,
        &info
    );

    if (err != ESP_OK) {
        return err;
    }

    snapshot->samples = info.total_frames;

    if (frame_count == 0) {
        return ESP_OK;
    }

    snapshot->latest_valid = true;
    snapshot->last_sample_timestamp_us = frame.timestamp_us;

    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        snapshot->latest_uv[channel] = frame.channel_uv[channel];
    }

    return ESP_OK;
}
