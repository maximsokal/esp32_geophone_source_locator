#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep the diagnostic accounting bounded and allocation-free in the timer
 * task. The final normal bucket represents 2044..2047 us; samples at or above
 * 2048 us are counted separately and never mixed into that bucket. */
#define KEYPAD_TIMING_HISTOGRAM_WIDTH_US 4U
#define KEYPAD_TIMING_HISTOGRAM_BUCKETS 512U

typedef struct {
    uint32_t value_us;
    bool overflow;
} keypad_timing_percentile_t;


static inline void keypad_timing_record_duration(
    uint32_t duration_us,
    uint32_t *count,
    uint32_t *min_us,
    uint32_t *max_us,
    uint64_t *sum_us,
    uint32_t *overflow,
    uint32_t *histogram
)
{
    (*count)++;
    if (duration_us < *min_us) {
        *min_us = duration_us;
    }
    if (duration_us > *max_us) {
        *max_us = duration_us;
    }
    *sum_us += duration_us;

    const uint32_t bucket = duration_us / KEYPAD_TIMING_HISTOGRAM_WIDTH_US;
    if (bucket >= KEYPAD_TIMING_HISTOGRAM_BUCKETS) {
        (*overflow)++;
        return;
    }
    histogram[bucket]++;
}


static inline keypad_timing_percentile_t keypad_timing_percentile_us(
    uint32_t count,
    uint32_t overflow,
    const uint32_t *histogram
)
{
    keypad_timing_percentile_t result = {
        .value_us = 0U,
        .overflow = false,
    };
    if (count == 0U) {
        return result;
    }

    /* ceil(count * 95 / 100), with the first sample as the minimum rank. */
    const uint32_t rank = (uint32_t)(
        ((uint64_t)count * 95ULL + 99ULL) / 100ULL
    );
    const uint32_t normal_count = overflow <= count
        ? count - overflow
        : 0U;

    /* The overflow counter is a right-censored tail. If the requested rank
     * lies in that tail, do not manufacture a precise p95 value. A malformed
     * overflow > count is treated the same way. */
    if (rank > normal_count) {
        result.overflow = true;
        return result;
    }

    uint64_t accumulated = 0U;
    for (uint32_t bucket = 0U;
         bucket < KEYPAD_TIMING_HISTOGRAM_BUCKETS;
         ++bucket) {
        accumulated += histogram[bucket];
        if (accumulated >= rank) {
            /* The bucket value is an upper bound. This remains a valid
             * 2047-us result when the rank lands in the final normal bucket. */
            result.value_us = (bucket + 1U) *
                KEYPAD_TIMING_HISTOGRAM_WIDTH_US - 1U;
            return result;
        }
    }

    /* A counter/histogram mismatch is treated as censored rather than
     * manufacturing a precise percentile. */
    result.overflow = true;
    return result;
}


static inline uint32_t keypad_timing_p95_censored_above_us(
    keypad_timing_percentile_t percentile
)
{
    return percentile.overflow
        ? KEYPAD_TIMING_HISTOGRAM_BUCKETS *
            KEYPAD_TIMING_HISTOGRAM_WIDTH_US
        : 0U;
}
