#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ad7606_driver.h"


#define AD7606_ACQUISITION_JITTER_BUCKET_COUNT 16U


typedef struct
{
    uint32_t count;
    uint32_t early_count;
    uint64_t min_us;
    uint64_t max_us;
    uint64_t sum_us;
    uint32_t histogram_us[AD7606_ACQUISITION_JITTER_BUCKET_COUNT];
    uint64_t period_us;

} ad7606_acquisition_jitter_t;


static inline uint32_t ad7606_acquisition_saturating_add_u32(
    uint32_t current,
    uint32_t increment
)
{
    return increment > UINT32_MAX - current
        ? UINT32_MAX
        : current + increment;
}


typedef struct
{
    bool running;
    bool latest_valid;

    uint64_t samples;
    uint64_t read_errors;
    uint32_t consecutive_failures;

    /* Acquisition-cadence observability for the current run. */
    uint64_t timer_ticks;
    uint64_t conversion_attempts;
    uint64_t collapsed_periods;
    uint64_t last_successful_sample_interval_us;
    uint64_t max_successful_sample_interval_us;
    ad7606_acquisition_jitter_t jitter;

    uint8_t os_ratio_configured;
    uint8_t os_ratio_measured;
    bool os_mismatch;

    bool serial_suspicious;
    ad7606_serial_pattern_t serial_pattern;
    ad7606_serial_quality_t serial_quality;
    ad7606_signal_quality_t signal_quality;
    ad7606_serial_stream_state_t stream_state;
    bool stream_stuck;
    uint32_t suspicious_streak;
    uint32_t repeated_frame_streak;
    uint32_t all_zero_frames;
    uint32_t all_ff_frames;
    uint32_t all_identical_frames;
    uint32_t varied_frames;
    uint32_t digital_interference_frames;
    uint32_t signal_rail_hit_frames;
    uint32_t signal_clip_frames;
    uint32_t quality_window_frames;
    uint32_t quality_window_bad_frames;
    uint32_t quality_window_all_zero_frames;
    uint32_t quality_window_all_ff_frames;
    uint32_t quality_window_all_identical_frames;
    uint32_t quality_window_repeated_frames;
    uint32_t quality_window_digital_interference_frames;
    uint32_t signal_quality_window_rail_hit_frames;
    uint32_t signal_quality_window_clip_frames;
    int busy_level;

    int64_t last_sample_timestamp_us;

    float latest_uv[AD7606_CHANNEL_COUNT];

    /* MAX-242 power interlock: last start-gate decision
     * (ad7606_power_gate_t), and a fault latched by a refused start or a
     * confirmed rail loss -- cleared only by the next explicit start. */
    int power_gate;
    bool power_fault_latched;
    float power_fault_rail_v;
    /* MAX-245: rail current when an over-current fault latched (A). */
    float power_fault_current_a;

} ad7606_acquisition_status_t;


esp_err_t ad7606_acquisition_start(void);

void ad7606_acquisition_stop(void);

esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
);
