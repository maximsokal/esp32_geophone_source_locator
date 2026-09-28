#include "ad7606_acquisition_timing.h"

#include <limits.h>
#include <stddef.h>


#define AD7606_ACQUISITION_TIMEBASE_US    1000000ULL
#define AD7606_TIMING_NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define AD7606_SERIAL_BITS_PER_FRAME       64U
#define AD7606_AFTER_READ_FIXED_MARGIN_NS  3000U
#define AD7606_DURING_READ_MARGIN_NS       4000U


typedef struct
{
    uint32_t oversampling_ratio;
    uint32_t tconv_max_ns;
    uint32_t max_throughput_hz;
} ad7606_os_timing_t;


/* AD7606 datasheet max tCONV and maximum throughput for each OS setting. */
static const ad7606_os_timing_t s_os_timing[] = {
    { 1U,  4150U, 200000U },
    { 2U,  9100U, 100000U },
    { 4U, 18800U,  50000U },
    { 8U, 39000U,  25000U },
};


uint64_t ad7606_acquisition_period_us(
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U) {
        return 0U;
    }

    return (
        AD7606_ACQUISITION_TIMEBASE_US +
        ((uint64_t)sample_rate_hz / 2U)
    ) /
        (uint64_t)sample_rate_hz;
}


uint64_t ad7606_acquisition_collapsed_periods(
    uint32_t notification_count
)
{
    return notification_count > 1U
        ? (uint64_t)notification_count - 1U
        : 0U;
}


uint64_t ad7606_acquisition_saturating_add_u64(
    uint64_t current,
    uint64_t increment
)
{
    if (increment > UINT64_MAX - current) {
        return UINT64_MAX;
    }

    return current + increment;
}


uint32_t ad7606_acquisition_ledc_duty_count(
    uint32_t sample_rate_hz,
    uint32_t duty_resolution_bits,
    uint32_t target_high_ns,
    uint32_t *actual_high_ns
)
{
    if (
        sample_rate_hz == 0U ||
        duty_resolution_bits == 0U ||
        duty_resolution_bits >= 32U ||
        target_high_ns == 0U ||
        actual_high_ns == NULL
    ) {
        return 0U;
    }

    const uint64_t period_counts = UINT64_C(1) << duty_resolution_bits;
    const uint64_t pulse_rate_product = (uint64_t)target_high_ns * sample_rate_hz;
    if (pulse_rate_product >
        (UINT64_MAX - AD7606_TIMING_NANOSECONDS_PER_SECOND + 1U) / period_counts) {
        return 0U;
    }
    const uint64_t numerator = pulse_rate_product * period_counts;
    uint64_t duty_count =
        (numerator + AD7606_TIMING_NANOSECONDS_PER_SECOND - 1U) /
        AD7606_TIMING_NANOSECONDS_PER_SECOND;
    if (duty_count == 0U) {
        duty_count = 1U;
    }
    if (duty_count >= period_counts) {
        return 0U;
    }

    const uint64_t actual_numerator =
        duty_count * AD7606_TIMING_NANOSECONDS_PER_SECOND;
    const uint64_t actual_denominator =
        (uint64_t)sample_rate_hz * period_counts;
    const uint64_t actual_ns =
        (actual_numerator + actual_denominator - 1U) / actual_denominator;
    if (actual_ns > UINT32_MAX) {
        return 0U;
    }

    *actual_high_ns = (uint32_t)actual_ns;
    return (uint32_t)duty_count;
}


uint32_t ad7606_acquisition_ledc_exact_resolution(
    uint32_t sample_rate_hz,
    uint32_t source_clock_hz,
    uint32_t maximum_resolution_bits
)
{
    if (sample_rate_hz == 0U || source_clock_hz == 0U || maximum_resolution_bits >= 32U) {
        return 0U;
    }

    const uint64_t scaled_clock = (uint64_t)source_clock_hz * 256U;
    for (uint32_t bits = maximum_resolution_bits; bits > 0U; --bits) {
        const uint64_t period_counts = UINT64_C(1) << bits;
        const uint64_t denominator = (uint64_t)sample_rate_hz * period_counts;
        if (denominator != 0U && scaled_clock % denominator == 0U &&
            scaled_clock / denominator >= 256U) {
            return bits;
        }
    }
    return 0U;
}


bool ad7606_acquisition_timing_resolve(
    uint32_t sample_rate_hz,
    uint32_t oversampling_ratio,
    uint32_t spi_clock_hz,
    ad7606_read_mode_t requested_mode,
    uint32_t convst_high_ns,
    ad7606_timing_result_t *result
)
{
    if (
        (sample_rate_hz != 500U && sample_rate_hz != 5000U &&
         sample_rate_hz != 10000U && sample_rate_hz != 20000U &&
         sample_rate_hz != 40000U) ||
        spi_clock_hz < 1000000U ||
        spi_clock_hz > 15000000U ||
        convst_high_ns < 1000U ||
        result == NULL
    ) {
        return false;
    }

    const ad7606_os_timing_t *os = NULL;
    for (size_t index = 0U; index < sizeof(s_os_timing) / sizeof(s_os_timing[0]); ++index) {
        if (s_os_timing[index].oversampling_ratio == oversampling_ratio) {
            os = &s_os_timing[index];
            break;
        }
    }
    if (os == NULL || sample_rate_hz > os->max_throughput_hz) {
        return false;
    }

    const uint64_t read_time_ns = (
        (uint64_t)AD7606_SERIAL_BITS_PER_FRAME *
        AD7606_TIMING_NANOSECONDS_PER_SECOND +
        (uint64_t)spi_clock_hz - 1U
    ) / (uint64_t)spi_clock_hz;
    const uint64_t after_margin_ns =
        AD7606_AFTER_READ_FIXED_MARGIN_NS + (uint64_t)convst_high_ns;
    const uint64_t after_time_ns =
        (uint64_t)os->tconv_max_ns + read_time_ns + after_margin_ns;
    const bool after_valid =
        after_time_ns * (uint64_t)sample_rate_hz <=
        AD7606_TIMING_NANOSECONDS_PER_SECOND;
    const bool during_valid =
        read_time_ns + AD7606_DURING_READ_MARGIN_NS <= os->tconv_max_ns &&
        ((uint64_t)os->tconv_max_ns + AD7606_DURING_READ_MARGIN_NS) *
            (uint64_t)sample_rate_hz <= AD7606_TIMING_NANOSECONDS_PER_SECOND;

    ad7606_read_mode_t resolved_mode = requested_mode;
    if (requested_mode == AD7606_READ_MODE_AUTO) {
        if (after_valid) {
            resolved_mode = AD7606_READ_MODE_AFTER_CONVERSION;
        }
        else if (during_valid) {
            resolved_mode = AD7606_READ_MODE_DURING_CONVERSION;
        }
        else {
            return false;
        }
    }
    else if (
        (requested_mode == AD7606_READ_MODE_AFTER_CONVERSION && !after_valid) ||
        (requested_mode == AD7606_READ_MODE_DURING_CONVERSION && !during_valid) ||
        (requested_mode != AD7606_READ_MODE_AFTER_CONVERSION &&
         requested_mode != AD7606_READ_MODE_DURING_CONVERSION)
    ) {
        return false;
    }

    *result = (ad7606_timing_result_t) {
        .mode = resolved_mode,
        .tconv_max_ns = os->tconv_max_ns,
        .read_time_ns = read_time_ns,
        .convst_high_ns = convst_high_ns,
    };
    return true;
}


bool ad7606_acquisition_timestamp_from_index(
    int64_t t0_us,
    uint64_t sample_index,
    uint32_t sample_rate_hz,
    int64_t *timestamp_us
)
{
    if (t0_us < 0 || sample_rate_hz == 0U || timestamp_us == NULL) {
        return false;
    }

    const uint64_t rate = sample_rate_hz;
    const uint64_t whole_seconds = sample_index / rate;
    const uint64_t remainder = sample_index % rate;
    if (whole_seconds > UINT64_MAX / AD7606_ACQUISITION_TIMEBASE_US) {
        return false;
    }

    const uint64_t whole_offset_us =
        whole_seconds * AD7606_ACQUISITION_TIMEBASE_US;
    const uint64_t remainder_offset_us =
        remainder * AD7606_ACQUISITION_TIMEBASE_US / rate;
    if (whole_offset_us > UINT64_MAX - remainder_offset_us) {
        return false;
    }
    const uint64_t offset_us = whole_offset_us + remainder_offset_us;
    if (offset_us > (uint64_t)INT64_MAX - (uint64_t)t0_us) {
        return false;
    }

    *timestamp_us = t0_us + (int64_t)offset_us;
    return true;
}


bool ad7606_acquisition_sample_index_from_pcnt(
    ad7606_read_mode_t mode,
    uint64_t completed_conversions,
    bool busy_high,
    uint64_t *sample_index,
    bool *discard
)
{
    if (sample_index == NULL || discard == NULL) {
        return false;
    }

    if (mode == AD7606_READ_MODE_AFTER_CONVERSION) {
        if (completed_conversions == 0U) {
            return false;
        }
        *sample_index = completed_conversions - 1U;
        *discard = false;
        return true;
    }
    if (mode != AD7606_READ_MODE_DURING_CONVERSION) {
        return false;
    }

    if (busy_high) {
        if (completed_conversions == 0U) {
            return false;
        }
        *sample_index = completed_conversions - 1U;
        *discard = false;
        return true;
    }
    if (completed_conversions < 2U) {
        return false;
    }
    /* BUSY fell before the task read; the previous output was overwritten. */
    *sample_index = completed_conversions - 2U;
    *discard = true;
    return true;
}


void ad7606_acquisition_count_extension_reset(
    ad7606_count_extension_t *state
)
{
    if (state != NULL) {
        *state = (ad7606_count_extension_t){0};
    }
}


bool ad7606_acquisition_count_extend_u32(
    ad7606_count_extension_t *state,
    uint32_t raw_count,
    uint64_t *extended_count
)
{
    if (state == NULL || extended_count == NULL) {
        return false;
    }

    if (!state->initialized) {
        state->last_raw_count = raw_count;
        state->extended_count = raw_count;
        state->initialized = true;
        *extended_count = state->extended_count;
        return true;
    }

    const uint32_t delta = raw_count - state->last_raw_count;
    if (delta > INT32_MAX) {
        return false;
    }

    state->extended_count = ad7606_acquisition_saturating_add_u64(
        state->extended_count,
        delta
    );
    state->last_raw_count = raw_count;
    *extended_count = state->extended_count;
    return true;
}


bool ad7606_acquisition_account_sample(
    ad7606_sample_accounting_t *state,
    uint64_t sample_index,
    bool sample_valid,
    uint64_t *new_missed_conversions
)
{
    if (state == NULL || sample_index == UINT64_MAX) {
        return false;
    }

    if (new_missed_conversions != NULL) {
        *new_missed_conversions = 0U;
    }
    if (!state->initialized) {
        state->initialized = true;
        state->next_sample_index = 0U;
    }
    if (sample_index < state->next_sample_index) {
        return false;
    }

    uint64_t missed = sample_index - state->next_sample_index;
    if (!sample_valid) {
        missed = ad7606_acquisition_saturating_add_u64(missed, 1U);
    }
    state->missed_conversions = ad7606_acquisition_saturating_add_u64(
        state->missed_conversions,
        missed
    );
    state->next_sample_index = sample_index + 1U;
    if (new_missed_conversions != NULL) {
        *new_missed_conversions = missed;
    }
    return sample_valid;
}
