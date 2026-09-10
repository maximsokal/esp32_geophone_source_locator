#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"


#define SEISMIC_ACQUISITION_MAX_CHANNELS  8


typedef struct
{
    /* Acquisition worker/backend state; this does not include ADC presence. */
    bool running;
    /* Physical source health (AD7606 BUSY presence for that backend). */
    bool source_healthy;
    bool latest_valid;

    const char *source_name;
    const char *transport_name;

    uint32_t sample_rate_hz;
    uint32_t bus_clock_hz;

    uint8_t channel_count;
    uint8_t bus_mode;
    uint8_t oversampling_ratio;

    int core_id;

    float input_range_v;

    uint64_t samples;
    uint64_t read_errors;
    uint32_t consecutive_failures;

    /* AD7606 serial observability. Other sources leave these at defaults. */
    bool serial_suspicious;
    const char *serial_pattern;
    const char *serial_quality;
    const char *signal_quality;
    uint32_t serial_suspicious_streak;
    uint32_t serial_repeated_frame_streak;
    uint32_t serial_all_zero_frames;
    uint32_t serial_all_ff_frames;
    uint32_t serial_all_identical_frames;
    uint32_t serial_varied_frames;
    uint32_t serial_digital_interference_frames;
    uint32_t signal_rail_hit_frames;
    uint32_t signal_clip_frames;
    uint32_t serial_quality_window_frames;
    uint32_t serial_quality_window_bad_frames;
    uint32_t serial_quality_window_all_zero_frames;
    uint32_t serial_quality_window_all_ff_frames;
    uint32_t serial_quality_window_all_identical_frames;
    uint32_t serial_quality_window_repeated_frames;
    uint32_t serial_quality_window_digital_interference_frames;
    uint32_t signal_quality_window_rail_hit_frames;
    uint32_t signal_quality_window_clip_frames;
    int serial_busy_level;

    int64_t last_sample_timestamp_us;

    float latest_uv[
        SEISMIC_ACQUISITION_MAX_CHANNELS
    ];

} seismic_acquisition_snapshot_t;


esp_err_t seismic_acquisition_start(void);

void seismic_acquisition_stop(void);

/* True while the selected acquisition task/backend is started. */
bool seismic_acquisition_is_running(void);

/* Physical/backend health independent from the cached device_status flag. */
bool seismic_acquisition_source_healthy(void);

/* Last start/probe failure; ESP_OK after a successful start. */
esp_err_t seismic_acquisition_last_error(void);

const char *seismic_acquisition_source_name(void);

esp_err_t seismic_acquisition_get_snapshot(
    seismic_acquisition_snapshot_t *snapshot
);
