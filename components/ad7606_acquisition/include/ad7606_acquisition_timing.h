#pragma once

#include <stdint.h>

#include <stdbool.h>


#define AD7606_OS1_TCONV_MIN_NS 2950U
#define AD7606_OS1_TCONV_MAX_NS 4650U
#define AD7606_OS2_TCONV_MIN_NS 7400U
#define AD7606_OS2_TCONV_MAX_NS 9600U
#define AD7606_OS4_TCONV_MIN_NS 15500U
#define AD7606_OS4_TCONV_MAX_NS 19300U
#define AD7606_OS8_TCONV_MIN_NS 32500U
#define AD7606_OS8_TCONV_MAX_NS 39500U
#define AD7606_OS_CLASSIFICATION_GAP_MIN_NS 1000U

_Static_assert(
    AD7606_OS1_TCONV_MAX_NS + AD7606_OS_CLASSIFICATION_GAP_MIN_NS <=
        AD7606_OS2_TCONV_MIN_NS,
    "AD7606 OS1 upper band plus its minimum gap must not overlap OS2"
);


typedef enum
{
    AD7606_READ_MODE_AUTO = 0,
    AD7606_READ_MODE_AFTER_CONVERSION,
    AD7606_READ_MODE_DURING_CONVERSION

} ad7606_read_mode_t;


typedef struct
{
    ad7606_read_mode_t mode;
    uint32_t tconv_max_ns;
    uint64_t read_time_ns;
    uint32_t convst_high_ns;

} ad7606_timing_result_t;


typedef struct
{
    uint32_t last_raw_count;
    uint64_t extended_count;
    bool initialized;

} ad7606_count_extension_t;


typedef struct
{
    uint64_t next_sample_index;
    uint64_t missed_conversions;
    bool initialized;

} ad7606_sample_accounting_t;


/*
 * esp_timer accepts an integral microsecond period. Round to the nearest
 * microsecond so a configured rate does not acquire a systematic positive
 * frequency error from integer truncation.
 *
 * A zero rate is invalid and returns zero instead of dividing by zero.
 */
uint64_t ad7606_acquisition_period_us(
    uint32_t sample_rate_hz
);


/* Number of configured periods lost when notifications were coalesced. */
uint64_t ad7606_acquisition_collapsed_periods(
    uint32_t notification_count
);


/* Saturating counter addition for long-running acquisition observability. */
uint64_t ad7606_acquisition_saturating_add_u64(
    uint64_t current,
    uint64_t increment
);

/* Quantizes a minimum CONVST high pulse to the selected LEDC resolution. */
uint32_t ad7606_acquisition_ledc_duty_count(
    uint32_t sample_rate_hz,
    uint32_t duty_resolution_bits,
    uint32_t target_high_ns,
    uint32_t *actual_high_ns
);

/* Chooses a supported LEDC resolution whose XTAL divider produces exact fs. */
uint32_t ad7606_acquisition_ledc_exact_resolution(
    uint32_t sample_rate_hz,
    uint32_t source_clock_hz,
    uint32_t maximum_resolution_bits
);

/* Resolves configured timing against the AD7606 tCONV and throughput limits. */
bool ad7606_acquisition_timing_resolve(
    uint32_t sample_rate_hz,
    uint32_t oversampling_ratio,
    uint32_t spi_clock_hz,
    ad7606_read_mode_t requested_mode,
    uint32_t convst_high_ns,
    ad7606_timing_result_t *result
);

/*
 * Resolve the measured OS ratio and require any valid configured plan to use
 * the same read mode. This lets a safe same-mode mismatch run while refusing
 * to use a mode chosen from the wrong hardware strap.
 */
bool ad7606_acquisition_timing_resolve_measured(
    uint32_t sample_rate_hz,
    uint32_t configured_oversampling_ratio,
    uint32_t measured_oversampling_ratio,
    uint32_t spi_clock_hz,
    ad7606_read_mode_t requested_mode,
    uint32_t convst_high_ns,
    ad7606_timing_result_t *result
);

/* Returns 1, 2, 4, or 8 for a calibrated BUSY width; zero means unknown. */
uint8_t ad7606_acquisition_classify_tconv_ns(uint32_t tconv_ns);

/* True only for the bounded OS1/OS2 gap eligible for degraded startup. */
bool ad7606_acquisition_tconv_is_unknown_degraded_window(uint32_t tconv_ns);

/* Exact-index timestamps avoid accumulated period rounding error. */
bool ad7606_acquisition_timestamp_from_index(
    int64_t t0_us,
    uint64_t sample_index,
    uint32_t sample_rate_hz,
    int64_t *timestamp_us
);

bool ad7606_acquisition_sample_index_from_pcnt(
    ad7606_read_mode_t mode,
    uint64_t completed_conversions,
    bool busy_high,
    uint64_t *sample_index,
    bool *discard
);

void ad7606_acquisition_count_extension_reset(
    ad7606_count_extension_t *state
);

bool ad7606_acquisition_count_extend_u32(
    ad7606_count_extension_t *state,
    uint32_t raw_count,
    uint64_t *extended_count
);

/* Returns true only when this candidate is a new publishable sample index. */
bool ad7606_acquisition_account_sample(
    ad7606_sample_accounting_t *state,
    uint64_t sample_index,
    bool sample_valid,
    uint64_t *new_missed_conversions
);
