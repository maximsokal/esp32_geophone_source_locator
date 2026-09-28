#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "ad7606_acquisition_timing.h"

enum {
    EXPECT_AFTER = 1U,
    EXPECT_DURING = 2U,
};

static const uint32_t s_rates[] = {500U, 5000U, 10000U, 20000U, 40000U};
static const uint32_t s_os[] = {1U, 2U, 4U, 8U};
static const uint32_t s_spi[] = {1000000U, 10000000U, 15000000U};
static const uint32_t s_resolutions[] = {14U, 12U, 11U, 10U, 9U};
static const uint32_t s_convst_high_ns[] = {2198U, 2149U, 2149U, 2149U, 2149U};

/* Per rate row, OS1..OS8, then SCLK1/10/15 MHz: after=1, during=2. */
static const uint8_t s_expected_modes[5][4][3] = {
    {{1U,1U,1U}, {1U,1U,3U}, {1U,3U,3U}, {1U,3U,3U}},
    {{1U,1U,1U}, {1U,1U,3U}, {1U,3U,3U}, {1U,3U,3U}},
    {{1U,1U,1U}, {1U,1U,3U}, {1U,3U,3U}, {0U,3U,3U}},
    {{0U,1U,1U}, {0U,1U,3U}, {0U,3U,3U}, {0U,2U,3U}},
    {{0U,1U,1U}, {0U,1U,3U}, {0U,2U,2U}, {0U,0U,0U}},
};


static uint64_t ceil_div_u64(uint64_t numerator, uint64_t denominator)
{
    return numerator / denominator + (numerator % denominator != 0U);
}


static void test_ledc_convst_pulse_quantization(void)
{
    for (size_t i = 0U; i < sizeof(s_rates) / sizeof(s_rates[0]); ++i) {
        const uint32_t bits = ad7606_acquisition_ledc_exact_resolution(
            s_rates[i], 40000000U, 14U);
        assert(bits == s_resolutions[i]);
        const uint64_t scaled_clock = UINT64_C(40000000) * 256U;
        const uint64_t divider_denominator =
            (uint64_t)s_rates[i] * (UINT64_C(1) << bits);
        assert(scaled_clock % divider_denominator == 0U);
    }
    assert(ad7606_acquisition_ledc_exact_resolution(0U, 40000000U, 14U) == 0U);
    assert(ad7606_acquisition_ledc_exact_resolution(500U, 0U, 14U) == 0U);

    for (size_t i = 0U; i < sizeof(s_rates) / sizeof(s_rates[0]); ++i) {
        uint32_t actual_ns = 0U;
        const uint32_t duty = ad7606_acquisition_ledc_duty_count(
            s_rates[i], s_resolutions[i], 2100U, &actual_ns);
        assert(duty == (i == 0U ? 18U : 44U));
        assert(actual_ns == s_convst_high_ns[i]);
        assert(actual_ns >= 1000U); /* firmware pulse exceeds the datasheet 25 ns min */
    }
    uint32_t actual_ns = 123U;
    assert(ad7606_acquisition_ledc_duty_count(0U, 14U, 2100U, &actual_ns) == 0U);
    assert(ad7606_acquisition_ledc_duty_count(500U, 0U, 2100U, &actual_ns) == 0U);
    assert(ad7606_acquisition_ledc_duty_count(500U, 32U, 2100U, &actual_ns) == 0U);
    assert(ad7606_acquisition_ledc_duty_count(500U, 14U, 0U, &actual_ns) == 0U);
    assert(ad7606_acquisition_ledc_duty_count(500U, 1U, 2100U, &actual_ns) == 1U);
    assert(actual_ns == 1000000U); /* coarse boundary resolution stays above target */
    assert(ad7606_acquisition_ledc_duty_count(500U, 14U, 3000000U, &actual_ns) == 0U);
    assert(ad7606_acquisition_ledc_duty_count(500U, 14U, 2100U, NULL) == 0U);
}


static void test_full_rate_os_clock_mode_matrix(void)
{
    unsigned checks = 0U;
    for (size_t r = 0U; r < 5U; ++r) {
        for (size_t o = 0U; o < 4U; ++o) {
            for (size_t c = 0U; c < 3U; ++c) {
                const uint8_t expected = s_expected_modes[r][o][c];
                const ad7606_read_mode_t requested[] = {
                    AD7606_READ_MODE_AUTO,
                    AD7606_READ_MODE_AFTER_CONVERSION,
                    AD7606_READ_MODE_DURING_CONVERSION,
                };
                for (size_t m = 0U; m < 3U; ++m) {
                    ad7606_timing_result_t result = {0};
                    const bool actual = ad7606_acquisition_timing_resolve(
                        s_rates[r], s_os[o], s_spi[c], requested[m],
                        s_convst_high_ns[r], &result);
                    const bool expected_valid = m == 0U
                        ? expected != 0U
                        : (expected & (m == 1U ? EXPECT_AFTER : EXPECT_DURING)) != 0U;
                    if (actual != expected_valid) {
                        printf("matrix mismatch rate=%u OS%u SCLK=%u request=%d expected=%d actual=%d mask=%u\n",
                            s_rates[r], s_os[o], s_spi[c], requested[m],
                            expected_valid, actual, expected);
                        fflush(stdout);
                    }
                    assert(actual == expected_valid);
                    if (expected_valid) {
                        const ad7606_read_mode_t expected_mode = m == 0U
                            ? ((expected & EXPECT_AFTER) != 0U
                                ? AD7606_READ_MODE_AFTER_CONVERSION
                                : AD7606_READ_MODE_DURING_CONVERSION)
                            : requested[m];
                        assert(result.mode == expected_mode);
                        assert(result.convst_high_ns == s_convst_high_ns[r]);
                        assert(result.read_time_ns == ceil_div_u64(
                            UINT64_C(64000000000), s_spi[c]));
                    }
                    ++checks;
                }
            }
        }
    }
    assert(checks == 180U);
    puts("Timing matrix: 60 rate/OS/SCLK combinations, 180 read-mode expectations");
}


static void test_default_20khz_os1_auto_reads_after_conversion(void)
{
    ad7606_timing_result_t result = {0};
    assert(ad7606_acquisition_timing_resolve(
        20000U,
        1U,
        10000000U,
        AD7606_READ_MODE_AUTO,
        2149U,
        &result
    ));
    assert(result.mode == AD7606_READ_MODE_AFTER_CONVERSION);
}


static void test_measured_busy_width_classification_and_timing_gate(void)
{
    assert(ad7606_acquisition_classify_tconv_ns(2950U) == 1U);
    assert(ad7606_acquisition_classify_tconv_ns(4650U) == 1U);
    assert(ad7606_acquisition_classify_tconv_ns(7400U) == 2U);
    assert(ad7606_acquisition_classify_tconv_ns(9600U) == 2U);
    assert(ad7606_acquisition_classify_tconv_ns(15500U) == 4U);
    assert(ad7606_acquisition_classify_tconv_ns(19300U) == 4U);
    assert(ad7606_acquisition_classify_tconv_ns(32500U) == 8U);
    assert(ad7606_acquisition_classify_tconv_ns(39500U) == 8U);
    assert(ad7606_acquisition_classify_tconv_ns(4000U) == 1U);
    assert(ad7606_acquisition_classify_tconv_ns(8500U) == 2U);
    assert(ad7606_acquisition_classify_tconv_ns(17000U) == 4U);
    assert(ad7606_acquisition_classify_tconv_ns(36000U) == 8U);
    assert(ad7606_acquisition_classify_tconv_ns(0U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(2949U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(4651U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(7399U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(9601U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(15499U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(19301U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(32499U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(39501U) == 0U);

    ad7606_timing_result_t result = {0};
    assert(ad7606_acquisition_timing_resolve_measured(
        20000U, 1U, 4U, 10000000U, AD7606_READ_MODE_AUTO, 2149U, &result));
    assert(result.mode == AD7606_READ_MODE_AFTER_CONVERSION);
    assert(!ad7606_acquisition_timing_resolve_measured(
        40000U, 1U, 4U, 10000000U, AD7606_READ_MODE_AUTO, 2149U, &result));
    assert(!ad7606_acquisition_timing_resolve_measured(
        40000U, 1U, 8U, 10000000U, AD7606_READ_MODE_AUTO, 2149U, &result));
    assert(!ad7606_acquisition_timing_resolve_measured(
        20000U, 1U, 0U, 10000000U, AD7606_READ_MODE_AUTO, 2149U, &result));
}


static void test_invalid_timing_boundaries(void)
{
    ad7606_timing_result_t result = {0};
    assert(!ad7606_acquisition_timing_resolve(0U, 1U, 10000000U,
        AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(750U, 1U, 10000000U,
        AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 3U, 10000000U,
        AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 1U, 999999U,
        AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 1U, 15000001U,
        AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 1U, 10000000U,
        AD7606_READ_MODE_AUTO, 999U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 1U, 10000000U,
        (ad7606_read_mode_t)3, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(500U, 1U, 10000000U,
        AD7606_READ_MODE_AUTO, 2100U, NULL));

    /* 40 kHz exceeds OS8's 25 kHz throughput; OS1 at 1 MHz cannot read in-period. */
    assert(!ad7606_acquisition_timing_resolve(40000U, 8U, 10000000U,
        AD7606_READ_MODE_AUTO, 2149U, &result));
    assert(!ad7606_acquisition_timing_resolve(40000U, 1U, 1000000U,
        AD7606_READ_MODE_AFTER_CONVERSION, 2149U, &result));
    assert(ad7606_acquisition_timing_resolve(40000U, 1U, 10000000U,
        AD7606_READ_MODE_AFTER_CONVERSION, 2149U, &result));
    assert(!ad7606_acquisition_timing_resolve(200001U, 1U, 15000000U,
        AD7606_READ_MODE_AUTO, 2149U, &result));
}


static void test_sample_index_timestamps_for_long_runs_and_overflow(void)
{
    int64_t timestamp = -1;
    const uint64_t one_day_at_40khz = UINT64_C(3456000000);
    assert(ad7606_acquisition_timestamp_from_index(
        123456, one_day_at_40khz, 40000U, &timestamp));
    assert(timestamp == INT64_C(86400123456));
    assert(ad7606_acquisition_timestamp_from_index(
        100U, 40000U, 40000U, &timestamp));
    assert(timestamp == 1000100);
    assert(!ad7606_acquisition_timestamp_from_index(-1, 0U, 40000U, &timestamp));
    assert(!ad7606_acquisition_timestamp_from_index(0, 1U, 0U, &timestamp));
    assert(!ad7606_acquisition_timestamp_from_index(0, 1U, 40000U, NULL));
    assert(!ad7606_acquisition_timestamp_from_index(
        INT64_MAX - 2, 1U, 40000U, &timestamp));
    assert(!ad7606_acquisition_timestamp_from_index(
        0, UINT64_MAX, 40000U, &timestamp));
}


static void test_pcnt_extension_wrap_restart_and_monotonicity(void)
{
    ad7606_count_extension_t count = {0};
    uint64_t extended = 0U;
    ad7606_acquisition_count_extension_reset(&count);
    assert(ad7606_acquisition_count_extend_u32(&count, UINT32_MAX - 1U, &extended));
    assert(extended == UINT32_MAX - 1U);
    assert(ad7606_acquisition_count_extend_u32(&count, 1U, &extended));
    assert(extended == UINT64_C(4294967297));
    assert(ad7606_acquisition_count_extend_u32(&count, 101U, &extended));
    assert(extended == UINT64_C(4294967397));

    const uint32_t previous_raw = count.last_raw_count;
    const uint64_t previous_extended = count.extended_count;
    assert(!ad7606_acquisition_count_extend_u32(&count, 100U, &extended));
    assert(count.last_raw_count == previous_raw);
    assert(count.extended_count == previous_extended);

    ad7606_acquisition_count_extension_reset(&count);
    assert(ad7606_acquisition_count_extend_u32(&count, 0U, &extended));
    assert(extended == 0U);
    assert(ad7606_acquisition_count_extend_u32(&count, 7U, &extended));
    assert(extended == 7U);
    assert(!ad7606_acquisition_count_extend_u32(NULL, 1U, &extended));
    assert(!ad7606_acquisition_count_extend_u32(&count, 1U, NULL));
}


static void test_pcnt_sample_index_by_read_mode(void)
{
    uint64_t index = UINT64_MAX;
    bool discard = true;
    assert(!ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_AFTER_CONVERSION, 0U, false, &index, &discard));
    assert(ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_AFTER_CONVERSION, 1U, true, &index, &discard));
    assert(index == 0U && !discard);
    assert(ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_DURING_CONVERSION, 1U, true, &index, &discard));
    assert(index == 0U && !discard);
    assert(!ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_DURING_CONVERSION, 1U, false, &index, &discard));
    assert(ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_DURING_CONVERSION, 2U, false, &index, &discard));
    assert(index == 0U && discard);
    assert(ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_DURING_CONVERSION, 2U, true, &index, &discard));
    assert(index == 1U && !discard);
    assert(!ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_AUTO, 2U, true, &index, &discard));
    assert(!ad7606_acquisition_sample_index_from_pcnt(
        AD7606_READ_MODE_DURING_CONVERSION, 2U, true, NULL, &discard));
}


static void test_missed_conversion_accounting_and_restart(void)
{
    ad7606_sample_accounting_t samples = {0};
    uint64_t missed = UINT64_MAX;
    assert(ad7606_acquisition_account_sample(&samples, 0U, true, &missed));
    assert(missed == 0U && samples.next_sample_index == 1U);
    assert(ad7606_acquisition_account_sample(&samples, 3U, true, &missed));
    assert(missed == 2U && samples.missed_conversions == 2U);
    assert(!ad7606_acquisition_account_sample(&samples, 4U, false, &missed));
    assert(missed == 1U && samples.missed_conversions == 3U);
    assert(ad7606_acquisition_account_sample(&samples, 5U, true, &missed));
    assert(missed == 0U && samples.next_sample_index == 6U);
    assert(!ad7606_acquisition_account_sample(&samples, 5U, true, &missed));
    assert(missed == 0U && samples.next_sample_index == 6U);
    assert(!ad7606_acquisition_account_sample(&samples, UINT64_MAX, true, &missed));

    samples = (ad7606_sample_accounting_t){0};
    assert(ad7606_acquisition_account_sample(&samples, 0U, true, &missed));
    assert(missed == 0U && samples.missed_conversions == 0U);
}


int main(void)
{
    test_ledc_convst_pulse_quantization();
    test_full_rate_os_clock_mode_matrix();
    test_default_20khz_os1_auto_reads_after_conversion();
    test_measured_busy_width_classification_and_timing_gate();
    test_invalid_timing_boundaries();
    test_sample_index_timestamps_for_long_runs_and_overflow();
    test_pcnt_extension_wrap_restart_and_monotonicity();
    test_pcnt_sample_index_by_read_mode();
    test_missed_conversion_accounting_and_restart();
    puts("AD7606 fast timing/accounting tests passed");
    return 0;
}
