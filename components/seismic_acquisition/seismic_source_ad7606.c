#include "seismic_source_driver.h"

#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h"

#include "ad7606_acquisition.h"
#include "ad7606_driver.h"
#include "ad7606_power_policy.h"
#include "app_task_config.h"
#include "hw_inventory.h"


static esp_err_t seismic_source_ad7606_start(void)
{
    esp_err_t err = ad7606_acquisition_start();
    if (err != ESP_OK) {
        return err;
    }

    err = ad7606_driver_presence_monitor_start();
    const char *presence_reason = "presence probe failed";
    if (err == ESP_OK) {
        presence_reason = "busy responds";
    } else if (err == ESP_ERR_NOT_FOUND) {
        presence_reason = "no BUSY response";
    }
    hw_inventory_report(
        HW_MODULE_AD7606,
        hw_inventory_classify_probe_error(err),
        presence_reason
    );

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
    /* A selected AD7606 backend supports cadence evidence, but a failed start
     * has observed no sampling period and must not be reported as zero-miss
     * proof. Preserve evidence after a completed/stopped run once at least one
     * real timer period was observed. */
    snapshot->cadence_available =
        status.running ||
        status.timer_ticks > 0U;
    snapshot->timer_ticks = status.timer_ticks;
    snapshot->conversion_attempts = status.conversion_attempts;
    snapshot->collapsed_periods = status.collapsed_periods;
    snapshot->last_successful_sample_interval_us =
        status.last_successful_sample_interval_us;
    snapshot->max_successful_sample_interval_us =
        status.max_successful_sample_interval_us;
    snapshot->jitter.count = status.jitter.count;
    snapshot->jitter.early_count = status.jitter.early_count;
    snapshot->jitter.min_us = status.jitter.min_us;
    snapshot->jitter.max_us = status.jitter.max_us;
    snapshot->jitter.sum_us = status.jitter.sum_us;
    snapshot->jitter.period_us = status.jitter.period_us;
    for (size_t bucket = 0; bucket < AD7606_ACQUISITION_JITTER_BUCKET_COUNT; ++bucket) {
        snapshot->jitter.histogram_us[bucket] = status.jitter.histogram_us[bucket];
    }
    snapshot->serial_suspicious = status.serial_suspicious;
    snapshot->serial_pattern = ad7606_serial_pattern_name(status.serial_pattern);
    snapshot->serial_quality = ad7606_serial_quality_name(status.serial_quality);
    snapshot->serial_stream_state =
        ad7606_serial_stream_state_name(status.stream_state);
    snapshot->serial_stream_stuck = status.stream_stuck;
    snapshot->power_fault_latched = status.power_fault_latched;
    snapshot->power_fault_rail_v = status.power_fault_rail_v;
    snapshot->power_fault_current_a = status.power_fault_current_a;
    snapshot->power_gate = ad7606_power_gate_name((ad7606_power_gate_t)status.power_gate);
    snapshot->signal_quality = ad7606_signal_quality_name(status.signal_quality);
    snapshot->serial_suspicious_streak = status.suspicious_streak;
    snapshot->serial_repeated_frame_streak = status.repeated_frame_streak;
    snapshot->serial_all_zero_frames = status.all_zero_frames;
    snapshot->serial_all_ff_frames = status.all_ff_frames;
    snapshot->serial_all_identical_frames = status.all_identical_frames;
    snapshot->serial_varied_frames = status.varied_frames;
    snapshot->serial_digital_interference_frames = status.digital_interference_frames;
    snapshot->signal_rail_hit_frames = status.signal_rail_hit_frames;
    snapshot->signal_clip_frames = status.signal_clip_frames;
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
    snapshot->signal_quality_window_rail_hit_frames =
        status.signal_quality_window_rail_hit_frames;
    snapshot->signal_quality_window_clip_frames =
        status.signal_quality_window_clip_frames;
    snapshot->serial_busy_level = status.busy_level;
    snapshot->last_sample_timestamp_us = status.last_sample_timestamp_us;

    snapshot->bus_clock_hz = CONFIG_AD7606_SPI_CLOCK_HZ;
    snapshot->bus_mode = AD7606_SPI_MODE;
    snapshot->input_range_v = AD7606_INPUT_RANGE_V;
    snapshot->oversampling_ratio = CONFIG_AD7606_OVERSAMPLING_RATIO;
    snapshot->os_ratio_configured = status.os_ratio_configured;
    snapshot->os_ratio_measured = status.os_ratio_measured;
    snapshot->os_mismatch = status.os_mismatch;

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
