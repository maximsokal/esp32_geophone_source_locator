#include "seismic_source_driver.h"

#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h"

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "app_task_config.h"


static esp_err_t seismic_source_ad7606_start(void)
{
    esp_err_t err = ad7606_acquisition_start();
    if (err != ESP_OK) {
        return err;
    }

    err = ad7606_driver_presence_monitor_start();
    if (err != ESP_OK) {
        ad7606_acquisition_stop();
        return err;
    }

    return ESP_OK;
}


static void seismic_source_ad7606_stop(void)
{
    ad7606_driver_presence_monitor_stop();
    ad7606_acquisition_stop();
}


static bool seismic_source_ad7606_is_healthy(void)
{
    return ad7606_driver_presence_is_healthy();
}


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
    snapshot->serial_suspicious = status.serial_suspicious;
    snapshot->serial_pattern = ad7606_serial_pattern_name(status.serial_pattern);
    snapshot->serial_quality = ad7606_serial_quality_name(status.serial_quality);
    snapshot->serial_suspicious_streak = status.suspicious_streak;
    snapshot->serial_repeated_frame_streak = status.repeated_frame_streak;
    snapshot->serial_all_zero_frames = status.all_zero_frames;
    snapshot->serial_all_ff_frames = status.all_ff_frames;
    snapshot->serial_all_identical_frames = status.all_identical_frames;
    snapshot->serial_varied_frames = status.varied_frames;
    snapshot->serial_digital_interference_frames = status.digital_interference_frames;
    snapshot->serial_rail_hit_frames = status.rail_hit_frames;
    snapshot->serial_quality_window_frames = status.quality_window_frames;
    snapshot->serial_quality_window_bad_frames = status.quality_window_bad_frames;
    snapshot->serial_quality_window_all_zero_frames =
        status.quality_window_all_zero_frames;
    snapshot->serial_quality_window_all_ff_frames =
        status.quality_window_all_ff_frames;
    snapshot->serial_quality_window_all_identical_frames =
        status.quality_window_all_identical_frames;
    snapshot->serial_quality_window_repeated_frames = status.quality_window_repeated_frames;
    snapshot->serial_quality_window_digital_interference_frames =
        status.quality_window_digital_interference_frames;
    snapshot->serial_quality_window_rail_hit_frames = status.quality_window_rail_hit_frames;
    snapshot->serial_busy_level = status.busy_level;
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
    .start = seismic_source_ad7606_start,
    .stop = seismic_source_ad7606_stop,
    .is_healthy = seismic_source_ad7606_is_healthy,
    .populate_snapshot = seismic_source_ad7606_snapshot,
};
