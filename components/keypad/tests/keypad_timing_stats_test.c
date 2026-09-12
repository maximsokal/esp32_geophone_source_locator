#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "keypad_timing_stats.h"


static void reset_stats(
    uint32_t *count,
    uint32_t *min_us,
    uint32_t *max_us,
    uint64_t *sum_us,
    uint32_t *overflow,
    uint32_t *histogram
)
{
    *count = 0U;
    *min_us = UINT32_MAX;
    *max_us = 0U;
    *sum_us = 0U;
    *overflow = 0U;
    for (uint32_t i = 0U; i < KEYPAD_TIMING_HISTOGRAM_BUCKETS; ++i) {
        histogram[i] = 0U;
    }
}


static void test_last_normal_bucket_is_not_censored(void)
{
    uint32_t count;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t overflow;
    uint32_t histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS];
    reset_stats(
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );

    keypad_timing_record_duration(
        2047U,
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );
    const keypad_timing_percentile_t p95 = keypad_timing_percentile_us(
        count,
        overflow,
        histogram
    );

    assert(count == 1U);
    assert(overflow == 0U);
    assert(histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS - 1U] == 1U);
    assert(!p95.overflow);
    assert(p95.value_us == 2047U);
    assert(keypad_timing_p95_censored_above_us(p95) == 0U);
}


static void test_overflow_tail_censors_p95(void)
{
    uint32_t count;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t overflow;
    uint32_t histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS];
    reset_stats(
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );

    keypad_timing_record_duration(
        2048U,
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );
    keypad_timing_record_duration(
        5100U,
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );
    const keypad_timing_percentile_t p95 = keypad_timing_percentile_us(
        count,
        overflow,
        histogram
    );

    assert(count == 2U);
    assert(overflow == 2U);
    assert(histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS - 1U] == 0U);
    assert(p95.overflow);
    assert(p95.value_us == 0U);
    assert(keypad_timing_p95_censored_above_us(p95) == 2048U);
}


static void test_small_overflow_tail_keeps_valid_p95(void)
{
    uint32_t count;
    uint32_t min_us;
    uint32_t max_us;
    uint64_t sum_us;
    uint32_t overflow;
    uint32_t histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS];
    reset_stats(
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );

    for (uint32_t i = 0U; i < 99U; ++i) {
        keypad_timing_record_duration(
            2047U,
            &count,
            &min_us,
            &max_us,
            &sum_us,
            &overflow,
            histogram
        );
    }
    keypad_timing_record_duration(
        5100U,
        &count,
        &min_us,
        &max_us,
        &sum_us,
        &overflow,
        histogram
    );
    const keypad_timing_percentile_t p95 = keypad_timing_percentile_us(
        count,
        overflow,
        histogram
    );

    assert(count == 100U);
    assert(overflow == 1U);
    assert(histogram[KEYPAD_TIMING_HISTOGRAM_BUCKETS - 1U] == 99U);
    assert(!p95.overflow);
    assert(p95.value_us == 2047U);
}


int main(void)
{
    test_last_normal_bucket_is_not_censored();
    test_overflow_tail_censors_p95();
    test_small_overflow_tail_keeps_valid_p95();
    puts("keypad timing histogram tests passed");
    return 0;
}
