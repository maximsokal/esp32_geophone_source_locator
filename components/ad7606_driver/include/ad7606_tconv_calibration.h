#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * MAX-435 HIL (2026-10-11): logic-analyzer BUSY width is 4.4-4.5 us, and
 * after accounting for GPIO-read overhead the cycle-counter estimator has
 * no measured residual bias. Keep the additive calibration at zero until a
 * real hardware measurement justifies a correction; do not tune it to the
 * host fake.
 */
#define AD7606_TCONV_MEASUREMENT_CALIBRATION_NS 0
#define AD7606_TCONV_MEASUREMENT_COUNT 3U

static inline uint32_t ad7606_tconv_apply_known_calibration_ns(
    uint32_t measured_tconv_ns
)
{
    const int64_t calibrated_ns = (int64_t)measured_tconv_ns -
        AD7606_TCONV_MEASUREMENT_CALIBRATION_NS;
    return calibrated_ns > UINT32_MAX
        ? UINT32_MAX
        : (calibrated_ns > 0 ? (uint32_t)calibrated_ns : 0U);
}

static inline uint32_t ad7606_tconv_apply_overhead_correction_ns(
    uint32_t raw_tconv_ns,
    uint32_t gpio_read_overhead_ns
)
{
    const int64_t corrected_ns = (int64_t)raw_tconv_ns -
        gpio_read_overhead_ns - AD7606_TCONV_MEASUREMENT_CALIBRATION_NS;
    return corrected_ns > UINT32_MAX
        ? UINT32_MAX
        : (corrected_ns > 0 ? (uint32_t)corrected_ns : 0U);
}

static inline uint32_t ad7606_tconv_median_ns(
    const uint32_t *measurements_ns,
    size_t count
)
{
    if (measurements_ns == NULL || count < 3U || count > 5U) {
        return 0U;
    }

    uint32_t ordered[5];
    for (size_t i = 0U; i < count; ++i) {
        ordered[i] = measurements_ns[i];
    }
    for (size_t i = 1U; i < count; ++i) {
        const uint32_t value = ordered[i];
        size_t j = i;
        while (j > 0U && ordered[j - 1U] > value) {
            ordered[j] = ordered[j - 1U];
            --j;
        }
        ordered[j] = value;
    }

    if ((count % 2U) != 0U) {
        return ordered[count / 2U];
    }
    return (uint32_t)(((uint64_t)ordered[count / 2U - 1U] +
        ordered[count / 2U]) / 2U);
}
