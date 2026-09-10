#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ad7606_driver.h"


typedef struct
{
    bool running;
    bool latest_valid;

    uint64_t samples;
    uint64_t read_errors;
    uint32_t consecutive_failures;

    bool serial_suspicious;
    ad7606_serial_pattern_t serial_pattern;
    ad7606_serial_quality_t serial_quality;
    uint32_t suspicious_streak;
    uint32_t repeated_frame_streak;
    uint32_t all_zero_frames;
    uint32_t all_ff_frames;
    uint32_t all_identical_frames;
    uint32_t varied_frames;
    uint32_t digital_interference_frames;
    uint32_t rail_hit_frames;
    uint32_t quality_window_frames;
    uint32_t quality_window_bad_frames;
    uint32_t quality_window_all_zero_frames;
    uint32_t quality_window_all_ff_frames;
    uint32_t quality_window_all_identical_frames;
    uint32_t quality_window_repeated_frames;
    uint32_t quality_window_digital_interference_frames;
    uint32_t quality_window_rail_hit_frames;
    int busy_level;

    int64_t last_sample_timestamp_us;

    float latest_uv[AD7606_CHANNEL_COUNT];

} ad7606_acquisition_status_t;


esp_err_t ad7606_acquisition_start(void);

void ad7606_acquisition_stop(void);

esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
);
