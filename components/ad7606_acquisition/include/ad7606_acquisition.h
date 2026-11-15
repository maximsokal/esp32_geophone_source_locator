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


esp_err_t ad7606_acquisition_probe(void);

esp_err_t ad7606_acquisition_start(void);

/* Sample rate of the next run (default: CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ). Changing it is refused while the source
 * runs (ESP_ERR_INVALID_STATE) or when the timing does not allow it (ESP_ERR_INVALID_ARG). */
esp_err_t ad7606_acquisition_set_sample_rate(uint32_t sample_rate_hz);
uint32_t ad7606_acquisition_get_sample_rate(void);
bool ad7606_acquisition_sample_rate_supported(uint32_t sample_rate_hz);

void ad7606_acquisition_stop(void);

esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
);


/*
 * One report per second of acquisition, produced by the Core 1 worker without any
 * logging or extra locking per sample. It tells whether the worker keeps up with the
 * conversions (conversions vs published) and where the cycles go. Cycle counts are CPU
 * cycles (cpu_mhz per microsecond).
 */
typedef struct
{
    uint32_t sequence;
    uint32_t window_ms;
    uint32_t cpu_mhz;

    uint32_t conversions;   /* BUSY conversions counted by the PCNT in the window */
    uint32_t published;     /* frames that reached the ring buffer */
    uint32_t attempts;      /* SPI reads started */
    uint32_t unstable;      /* reads discarded because a conversion finished during the read */
    uint32_t read_errors;
    uint32_t gaps;          /* conversion periods never read (collapsed) */

    uint32_t loops;         /* worker wake-ups handled */
    uint32_t notify_total;  /* BUSY notifications consumed by those wake-ups */
    uint32_t notify_max;    /* largest backlog found by one wake-up */

    uint32_t isr_count;
    uint32_t isr_cycles_avg;
    uint32_t isr_cycles_max;
    uint32_t loop_cycles_avg;
    uint32_t loop_cycles_max;
    uint32_t spi_cycles_avg;
    uint32_t spi_cycles_max;
    uint32_t publish_cycles_avg;
    uint32_t publish_cycles_max;

    /*
     * Raw ADC codes per channel over the window, from accumulators the worker already feeds
     * once per frame: tells a flat or saturated geophone channel from a quiet one without any
     * per-frame logging. Only frames the worker actually processed are counted.
     */
    int32_t ch_mean_raw[AD7606_CHANNEL_COUNT];
    uint32_t ch_p2p_raw[AD7606_CHANNEL_COUNT];
    uint32_t ch_rms_raw_x10[AD7606_CHANNEL_COUNT];  /* AC RMS about the mean, tenths of a code */
    uint32_t ch_saturated[AD7606_CHANNEL_COUNT];

} ad7606_rate_report_t;

/* Returns true once per new report; safe from any task. */
bool ad7606_acquisition_take_rate_report(ad7606_rate_report_t *report);

/*
 * Prints whatever the worker has handed over since the last call, from the caller's
 * context and never from the worker: the per-second AD7606_RATE line, the AD7606_DIAG
 * CH1..CH4 block (first window, on a VAR/FLAT/CLIP/EMPTY change, otherwise every tenth
 * window) and the newest AD7606_SERIAL_DIAG frame. Call it about once per second from a
 * service-core task; a report that is not collected in time is replaced by the next one.
 */
void ad7606_acquisition_log_rate_report(void);
