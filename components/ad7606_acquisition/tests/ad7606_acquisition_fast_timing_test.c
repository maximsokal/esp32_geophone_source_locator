#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../ad7606_driver/include/ad7606_tconv_calibration.h"
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
static unsigned s_max435_failures;


static void tol_1_classification_band_edges(void)
{
    assert(ad7606_acquisition_classify_tconv_ns(2949U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(2950U) == 1U);
    assert(ad7606_acquisition_classify_tconv_ns(4650U) == 1U);
    assert(ad7606_acquisition_classify_tconv_ns(4651U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(7399U) == 0U);
    assert(ad7606_acquisition_classify_tconv_ns(7400U) == 2U);
    assert(ad7606_acquisition_classify_tconv_ns(9600U) == 2U);
    assert(ad7606_acquisition_classify_tconv_ns(9601U) == 0U);
}

static void tol_1_degraded_window_is_exact(void)
{
    for (uint32_t tconv_ns = 0U; tconv_ns <= 10000U; ++tconv_ns) {
        const bool expected = tconv_ns >= 4651U && tconv_ns <= 7399U;
        assert(ad7606_acquisition_tconv_is_unknown_degraded_window(tconv_ns) ==
            expected);
    }
}

static void tol_1_band_edges_and_degraded_window_exact(void)
{
    tol_1_classification_band_edges();
    tol_1_degraded_window_is_exact();
    puts("MAX435 TOL-1 PASS");
}


static void tol_3_gpio_read_overhead_subtracted_once_with_150ns_bound(void)
{
    static const uint32_t true_widths_ns[] = {4150U, 4400U, 4500U};
    static const uint32_t per_poll_gpio_read_ns[] = {0U, 488U, 600U};
    assert(AD7606_TCONV_MEASUREMENT_CALIBRATION_NS == 0);

    for (size_t width_index = 0U;
        width_index < sizeof(true_widths_ns) / sizeof(true_widths_ns[0]);
        ++width_index) {
        for (size_t overhead_index = 0U;
            overhead_index < sizeof(per_poll_gpio_read_ns) /
                sizeof(per_poll_gpio_read_ns[0]);
            ++overhead_index) {
            const uint32_t true_width_ns = true_widths_ns[width_index];
            const uint32_t per_poll_overhead_ns =
                per_poll_gpio_read_ns[overhead_index];
            const uint32_t measured_overhead_ns = per_poll_overhead_ns * 2U;
            /* Rising and falling edge polls each contribute one fake read cost. */
            const uint32_t raw_after_poll_overhead_ns =
                true_width_ns + measured_overhead_ns;
            const uint32_t corrected_ns =
                ad7606_tconv_apply_overhead_correction_ns(
                    raw_after_poll_overhead_ns,
                    measured_overhead_ns);
            const uint32_t error_ns = corrected_ns > true_width_ns
                ? corrected_ns - true_width_ns
                : true_width_ns - corrected_ns;
            assert(error_ns <= 150U);
            assert(raw_after_poll_overhead_ns - corrected_ns ==
                measured_overhead_ns);
            if (measured_overhead_ns != 0U) {
                const uint32_t corrected_twice_ns =
                    ad7606_tconv_apply_overhead_correction_ns(
                        corrected_ns,
                        measured_overhead_ns);
                assert(corrected_twice_ns != corrected_ns);
            }
        }
    }
    puts("MAX435 TOL-3 PASS");
}

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


static void tconv_4500_ns_after_overhead_subtraction_classifies_os1(void)
{
    const uint32_t measured_ns =
        ad7606_tconv_apply_overhead_correction_ns(5700U, 1200U);
    const bool passed = AD7606_TCONV_MEASUREMENT_CALIBRATION_NS == 0 &&
        measured_ns == 4500U &&
        ad7606_acquisition_classify_tconv_ns(measured_ns) == 1U;
    if (passed) {
        puts("MAX435 T2 PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 T2 FAIL calibration=%d measured=%u class=%u\n",
            AD7606_TCONV_MEASUREMENT_CALIBRATION_NS,
            (unsigned)measured_ns,
            (unsigned)ad7606_acquisition_classify_tconv_ns(measured_ns));
    }
}


static void classify_bands_do_not_overlap_and_os1_upper_below_os2_lower(void)
{
    assert(AD7606_OS1_TCONV_MIN_NS < AD7606_OS1_TCONV_MAX_NS);
    assert(AD7606_OS1_TCONV_MAX_NS < AD7606_OS2_TCONV_MIN_NS);
    assert(AD7606_OS2_TCONV_MIN_NS < AD7606_OS2_TCONV_MAX_NS);
    assert(AD7606_OS2_TCONV_MIN_NS - AD7606_OS1_TCONV_MAX_NS >=
        AD7606_OS_CLASSIFICATION_GAP_MIN_NS);
    assert(ad7606_acquisition_tconv_is_unknown_degraded_window(
        AD7606_OS1_TCONV_MAX_NS + 1U));
    assert(ad7606_acquisition_tconv_is_unknown_degraded_window(
        AD7606_OS2_TCONV_MIN_NS - 1U));
    assert(!ad7606_acquisition_tconv_is_unknown_degraded_window(
        AD7606_OS1_TCONV_MAX_NS));
    assert(!ad7606_acquisition_tconv_is_unknown_degraded_window(
        AD7606_OS2_TCONV_MIN_NS));
}


static void classify_rejects_value_between_bands_6000_to_7000(void)
{
    for (uint32_t tconv_ns = 6000U; tconv_ns <= 7000U; ++tconv_ns) {
        assert(ad7606_acquisition_classify_tconv_ns(tconv_ns) == 0U);
    }
    assert(ad7606_acquisition_classify_tconv_ns(
        ad7606_tconv_apply_overhead_correction_ns(6500U, 0U)) == 0U);
}


static void median_of_three_discards_single_outlier(void)
{
    const uint32_t raw_measurements_ns[] = {6688U, 4500U, 4500U};
    uint32_t measurements_ns[
        sizeof(raw_measurements_ns) / sizeof(raw_measurements_ns[0])];
    for (size_t i = 0U; i <
        sizeof(raw_measurements_ns) / sizeof(raw_measurements_ns[0]); ++i) {
        measurements_ns[i] =
            ad7606_tconv_apply_known_calibration_ns(raw_measurements_ns[i]);
    }
    const uint32_t median_ns = ad7606_tconv_median_ns(
        measurements_ns,
        sizeof(measurements_ns) / sizeof(measurements_ns[0]));
    const bool passed = median_ns == 4500U &&
        ad7606_acquisition_classify_tconv_ns(median_ns) == 1U;
    if (passed) {
        puts("MAX435 T5 PASS");
    }
    else {
        ++s_max435_failures;
        printf("MAX435 T5 FAIL median=%u class=%u\n",
            (unsigned)median_ns,
            (unsigned)ad7606_acquisition_classify_tconv_ns(median_ns));
    }
}


static void tol_6_outlier_discard_is_order_independent(void)
{
    static const uint32_t one_outlier_orders_ns[3][3] = {
        {6688U, 4500U, 4500U},
        {4500U, 6688U, 4500U},
        {4500U, 4500U, 6688U},
    };
    for (size_t order = 0U; order < 3U; ++order) {
        uint32_t corrected[3];
        for (size_t sample = 0U; sample < 3U; ++sample) {
            corrected[sample] = ad7606_tconv_apply_known_calibration_ns(
                one_outlier_orders_ns[order][sample]);
        }
        const uint32_t median_ns = ad7606_tconv_median_ns(corrected, 3U);
        assert(median_ns == 4500U);
        assert(ad7606_acquisition_classify_tconv_ns(median_ns) == 1U);
    }

    /* Two high samples are not a lone preemption artifact: do not accept OS1. */
    const uint32_t two_outliers_ns[] = {6688U, 6700U, 4500U};
    uint32_t corrected_two_outliers[3];
    for (size_t sample = 0U; sample < 3U; ++sample) {
        corrected_two_outliers[sample] =
            ad7606_tconv_apply_known_calibration_ns(two_outliers_ns[sample]);
    }
    const uint32_t median_ns = ad7606_tconv_median_ns(
        corrected_two_outliers,
        3U);
    assert(median_ns > AD7606_OS1_TCONV_MAX_NS);
    assert(ad7606_acquisition_classify_tconv_ns(median_ns) != 1U);
    puts("MAX435 TOL-6 PASS");
}


static void tol_7_convst_pulse_and_pwm_period_are_pinned(void)
{
    static const uint32_t rates_hz[] = {500U, 20000U};
    const uint64_t scaled_clock = UINT64_C(40000000) * 256U;
    for (size_t i = 0U; i < sizeof(rates_hz) / sizeof(rates_hz[0]); ++i) {
        const uint32_t rate_hz = rates_hz[i];
        const uint32_t bits = ad7606_acquisition_ledc_exact_resolution(
            rate_hz,
            40000000U,
            14U);
        uint32_t convst_high_ns = 0U;
        const uint32_t duty = ad7606_acquisition_ledc_duty_count(
            rate_hz,
            bits,
            2100U,
            &convst_high_ns);
        assert(bits != 0U && duty != 0U);
        assert(convst_high_ns >= 1000U && convst_high_ns <= 4000U);

        const uint64_t period_counts = UINT64_C(1) << bits;
        const uint64_t divider_denominator = (uint64_t)rate_hz * period_counts;
        assert(divider_denominator != 0U);
        assert(scaled_clock % divider_denominator == 0U);
        const uint64_t clock_divider = scaled_clock / divider_denominator;
        const uint64_t actual_period_numerator =
            clock_divider * period_counts;
        const uint64_t target_period_numerator =
            scaled_clock / rate_hz;
        const uint64_t period_error_numerator =
            actual_period_numerator > target_period_numerator
                ? actual_period_numerator - target_period_numerator
                : target_period_numerator - actual_period_numerator;
        assert(period_error_numerator * UINT64_C(1000000) <=
            target_period_numerator);
    }
    puts("MAX435 TOL-7 PASS");
}


static void test_run_time_presets_have_exact_convst_and_valid_timing(void)
{
    const uint32_t rates[] = { 500U, 1000U, 2000U, 4000U, 5000U, 8000U, 10000U, 20000U };
    for (size_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        ad7606_timing_result_t result = {0};
        assert(ad7606_acquisition_timing_resolve(rates[i], 1U, 10000000U, AD7606_READ_MODE_AUTO, 2100U, &result));
        /* The XTAL LEDC must hit the rate exactly, or the sample times drift. */
        assert(ad7606_acquisition_ledc_exact_resolution(rates[i], 40000000U, 14U) != 0U);
    }
    ad7606_timing_result_t result = {0};
    assert(!ad7606_acquisition_timing_resolve(3000U, 1U, 10000000U, AD7606_READ_MODE_AUTO, 2100U, &result));
    assert(!ad7606_acquisition_timing_resolve(250000U, 1U, 10000000U, AD7606_READ_MODE_AUTO, 2100U, &result));
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


/*
 * HIL (MAX-435): this board's BUSY is 4.4-4.5 us, above the 4.15 us datasheet
 * maximum used for the read budget, and the log shows both numbers. That is
 * accepted only because the classification band is wider than the datasheet
 * figure, so the per-sample budget must still hold with the widest BUSY width
 * the band lets through, for every mode the resolver accepts.
 */
static uint32_t band_max_tconv_ns(uint32_t oversampling)
{
    switch (oversampling) {
    case 1U: return AD7606_OS1_TCONV_MAX_NS;
    case 2U: return AD7606_OS2_TCONV_MAX_NS;
    case 4U: return AD7606_OS4_TCONV_MAX_NS;
    default: return AD7606_OS8_TCONV_MAX_NS;
    }
}

static void measured_busy_above_datasheet_max_keeps_read_budget(void)
{
    ad7606_timing_result_t result = {0};

    /* The value from the failing field log: 4525 ns at OS1, 20 kHz. */
    assert(ad7606_acquisition_classify_tconv_ns(4525U) == 1U);
    assert(ad7606_acquisition_timing_resolve_measured(
        20000U, 1U, 1U, 10000000U, AD7606_READ_MODE_AUTO, 2149U, &result));
    assert(result.mode == AD7606_READ_MODE_AFTER_CONVERSION);
    assert(result.tconv_max_ns == 4150U);
    assert(4525U > result.tconv_max_ns);

    unsigned accepted_after = 0U;
    for (size_t rate = 0U; rate < sizeof(s_rates) / sizeof(s_rates[0]); ++rate) {
        for (size_t os = 0U; os < sizeof(s_os) / sizeof(s_os[0]); ++os) {
            for (size_t spi = 0U; spi < sizeof(s_spi) / sizeof(s_spi[0]); ++spi) {
                for (size_t high = 0U;
                     high < sizeof(s_convst_high_ns) / sizeof(s_convst_high_ns[0]);
                     ++high) {
                    if (!ad7606_acquisition_timing_resolve(
                            s_rates[rate], s_os[os], s_spi[spi],
                            AD7606_READ_MODE_AUTO, s_convst_high_ns[high], &result) ||
                        result.mode != AD7606_READ_MODE_AFTER_CONVERSION) {
                        continue;
                    }
                    ++accepted_after;
                    const uint64_t worst_ns = (uint64_t)band_max_tconv_ns(s_os[os]) +
                        result.read_time_ns + 3000U + s_convst_high_ns[high];
                    assert(worst_ns * s_rates[rate] <= UINT64_C(1000000000));
                }
            }
        }
    }
    assert(accepted_after > 0U);
}


int main(void)
{
#if defined(AD7606_TCONV_TOL1_BANDS_ONLY)
    tol_1_classification_band_edges();
    return 0;
#elif defined(AD7606_TCONV_TOL3_ONLY)
    tol_3_gpio_read_overhead_subtracted_once_with_150ns_bound();
    return 0;
#elif defined(AD7606_TCONV_TOL7_ONLY)
    tol_7_convst_pulse_and_pwm_period_are_pinned();
    return 0;
#elif defined(AD7606_TCONV_TOL_ONLY)
    tol_1_band_edges_and_degraded_window_exact();
    tol_3_gpio_read_overhead_subtracted_once_with_150ns_bound();
    tol_6_outlier_discard_is_order_independent();
    tol_7_convst_pulse_and_pwm_period_are_pinned();
    return s_max435_failures == 0U ? 0 : 1;
#else
    test_ledc_convst_pulse_quantization();
    test_full_rate_os_clock_mode_matrix();
    test_default_20khz_os1_auto_reads_after_conversion();
    test_measured_busy_width_classification_and_timing_gate();
    measured_busy_above_datasheet_max_keeps_read_budget();
    test_run_time_presets_have_exact_convst_and_valid_timing();
    test_invalid_timing_boundaries();
    test_sample_index_timestamps_for_long_runs_and_overflow();
    test_pcnt_extension_wrap_restart_and_monotonicity();
    test_pcnt_sample_index_by_read_mode();
    test_missed_conversion_accounting_and_restart();
    tconv_4500_ns_after_overhead_subtraction_classifies_os1();
    classify_bands_do_not_overlap_and_os1_upper_below_os2_lower();
    classify_rejects_value_between_bands_6000_to_7000();
    median_of_three_discards_single_outlier();
    tol_1_band_edges_and_degraded_window_exact();
    tol_3_gpio_read_overhead_subtracted_once_with_150ns_bound();
    tol_6_outlier_discard_is_order_independent();
    tol_7_convst_pulse_and_pwm_period_are_pinned();
    puts("AD7606 fast timing/accounting tests passed");
    return s_max435_failures == 0U ? 0 : 1;
#endif
}
