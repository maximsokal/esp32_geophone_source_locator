#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "geophone_events.h"


#define TWO_PI 6.28318530717958647692f


static void fill_background_sine(
    float *buffer,
    size_t start,
    size_t count,
    float amplitude,
    float cycles_per_sample
)
{
    for (size_t i = 0u; i < count; ++i) {
        buffer[start + i] =
            amplitude * sinf(TWO_PI * cycles_per_sample * (float)i);
    }
}


static void insert_rect_pulse(
    float *buffer,
    size_t start,
    size_t width,
    float amplitude
)
{
    for (size_t i = 0u; i < width; ++i) {
        buffer[start + i] = amplitude;
    }
}


static void insert_triangle_pulse(
    float *buffer,
    size_t buffer_len,
    size_t center,
    size_t half_width,
    float amplitude
)
{
    for (size_t offset = 0u; offset <= half_width; ++offset) {
        const float value =
            amplitude * (1.0f - (float)offset / (float)half_width);

        if (center + offset < buffer_len) {
            buffer[center + offset] = value;
        }
        if (offset <= center && center - offset < buffer_len) {
            buffer[center - offset] = value;
        }
    }
}


/* ---------------------------------------------------------------- */
/* Energy/RMS trigger baseline                                      */
/* ---------------------------------------------------------------- */

static void test_energy_trigger_finds_impulse_after_quiet_baseline(void)
{
    float samples[300] = { 0 };
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    const geophone_energy_trigger_config_t config = {
        .noise_window_samples = 50u,
        .trigger_window_samples = 10u,
        .threshold_multiplier = 5.0f
    };
    geophone_energy_trigger_result_t result;

    assert(geophone_event_energy_trigger_detect(samples, 300u, &config, &result));
    assert(result.found);
    assert(result.trigger_index == 191u);
}


static void test_energy_trigger_rejects_invalid_arguments(void)
{
    float samples[10] = { 0 };
    const geophone_energy_trigger_config_t config = {
        .noise_window_samples = 5u,
        .trigger_window_samples = 5u,
        .threshold_multiplier = 3.0f
    };
    geophone_energy_trigger_result_t result;

    assert(!geophone_event_energy_trigger_detect(NULL, 10u, &config, &result));
    assert(!geophone_event_energy_trigger_detect(samples, 10u, NULL, &result));
    assert(!geophone_event_energy_trigger_detect(samples, 3u, &config, &result));
}


static void test_energy_trigger_never_fires_on_pure_silence(void)
{
    float samples[300] = { 0 };
    const geophone_energy_trigger_config_t config = {
        .noise_window_samples = 50u,
        .trigger_window_samples = 10u,
        .threshold_multiplier = 5.0f
    };
    geophone_energy_trigger_result_t result;

    assert(!geophone_event_energy_trigger_detect(samples, 300u, &config, &result));
    assert(!result.found);
}


static void test_geophone_math_rejects_nonfinite_inputs(void)
{
    float samples[16] = { 0.0f };
    geophone_energy_trigger_config_t energy_config = {
        .noise_window_samples = 4u,
        .trigger_window_samples = 4u,
        .threshold_multiplier = 3.0f
    };
    geophone_energy_trigger_result_t energy_result;

    energy_config.threshold_multiplier = NAN;
    assert(!geophone_event_energy_trigger_detect(
        samples, 16u, &energy_config, &energy_result
    ));
    energy_config.threshold_multiplier = INFINITY;
    assert(!geophone_event_energy_trigger_detect(
        samples, 16u, &energy_config, &energy_result
    ));
    energy_config.threshold_multiplier = 0.0f;
    assert(!geophone_event_energy_trigger_detect(
        samples, 16u, &energy_config, &energy_result
    ));

    samples[3] = NAN;
    energy_config.threshold_multiplier = 3.0f;
    assert(!geophone_event_energy_trigger_detect(
        samples, 16u, &energy_config, &energy_result
    ));

    geophone_sta_lta_config_t sta_lta_config = {
        .sta_window_samples = 2u,
        .lta_window_samples = 8u,
        .trigger_ratio = 3.0f
    };
    geophone_sta_lta_result_t sta_lta_result;
    samples[3] = 0.0f;
    sta_lta_config.trigger_ratio = NAN;
    assert(!geophone_event_sta_lta_detect(
        samples, 16u, &sta_lta_config, &sta_lta_result
    ));
    sta_lta_config.trigger_ratio = 0.0f;
    assert(!geophone_event_sta_lta_detect(
        samples, 16u, &sta_lta_config, &sta_lta_result
    ));
    sta_lta_config.trigger_ratio = 3.0f;
    samples[4] = INFINITY;
    assert(!geophone_event_sta_lta_detect(
        samples, 16u, &sta_lta_config, &sta_lta_result
    ));

    geophone_arrival_refine_config_t refine_config = {
        .lookback_samples = 2u,
        .lookahead_samples = 2u,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t refine_result;
    samples[4] = 0.0f;
    refine_config.amplitude_fraction = NAN;
    assert(!geophone_event_refine_arrival(
        samples, 16u, 4u, &refine_config, &refine_result
    ));
    refine_config.amplitude_fraction = 1.1f;
    assert(!geophone_event_refine_arrival(
        samples, 16u, 4u, &refine_config, &refine_result
    ));
    refine_config.amplitude_fraction = 0.5f;
    samples[4] = -INFINITY;
    assert(!geophone_event_refine_arrival(
        samples, 16u, 4u, &refine_config, &refine_result
    ));

    float channel_b[16] = { 0.0f };
    geophone_cross_correlation_config_t correlation_config = {
        .max_lag_samples = 2u
    };
    geophone_cross_correlation_result_t correlation_result;
    samples[4] = 1.0f;
    channel_b[4] = 1.0f;
    channel_b[5] = NAN;
    assert(!geophone_event_cross_correlate(
        samples, channel_b, 16u, &correlation_config, &correlation_result
    ));

    samples[4] = 0.0f;
    samples[5] = 0.0f;
    assert(geophone_event_compute_snr(samples, 16u, 4u, 4u, 4u) == 0.0f);

    const geophone_pick_quality_config_t quality_config = {
        .minimum_snr = 5.0f
    };
    assert(!geophone_event_pick_is_acceptable(NAN, &quality_config));
    assert(!geophone_event_pick_is_acceptable(INFINITY, &quality_config));

    const geophone_pick_quality_config_t nan_threshold = {
        .minimum_snr = NAN
    };
    assert(!geophone_event_pick_is_acceptable(5.0f, &nan_threshold));

    const geophone_pick_quality_config_t negative_threshold = {
        .minimum_snr = -1.0f
    };
    assert(!geophone_event_pick_is_acceptable(5.0f, &negative_threshold));
}


static void test_geophone_math_rejects_window_overflow_and_clamps_edges(void)
{
    float samples[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
    geophone_energy_trigger_config_t energy_config = {
        .noise_window_samples = SIZE_MAX,
        .trigger_window_samples = 1u,
        .threshold_multiplier = 3.0f
    };
    geophone_energy_trigger_result_t energy_result;
    assert(!geophone_event_energy_trigger_detect(
        samples, 4u, &energy_config, &energy_result
    ));

    const geophone_arrival_refine_config_t refine_config = {
        .lookback_samples = 0u,
        .lookahead_samples = SIZE_MAX,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t refine_result;
    assert(geophone_event_refine_arrival(
        samples, 4u, 2u, &refine_config, &refine_result
    ));
    assert(refine_result.refined_index == 2u);

    const float snr = geophone_event_compute_snr(
        samples, 4u, 1u, 2u, SIZE_MAX
    );
    assert(snr == 1.0e6f);
    assert(isfinite(snr));
}


/* ---------------------------------------------------------------- */
/* STA/LTA adaptive onset detector                                   */
/* ---------------------------------------------------------------- */

static void test_sta_lta_finds_impulse_over_quiet_background(void)
{
    float samples[300] = { 0 };
    fill_background_sine(samples, 0u, 300u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    const geophone_sta_lta_config_t config = {
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .trigger_ratio = 3.0f
    };
    geophone_sta_lta_result_t result;

    assert(geophone_event_sta_lta_detect(samples, 300u, &config, &result));
    assert(result.found);
    assert(result.trigger_index == 191u);
    assert(result.ratio_at_trigger >= 3.0f);
}


static void test_sta_lta_missing_channel_never_triggers(void)
{
    float samples[300] = { 0 };
    const geophone_sta_lta_config_t config = {
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .trigger_ratio = 3.0f
    };
    geophone_sta_lta_result_t result;

    assert(!geophone_event_sta_lta_detect(samples, 300u, &config, &result));
    assert(!result.found);
}


static void test_sta_lta_picks_earliest_arrival_not_the_larger_echo(void)
{
    /* Multipath-like scenario: a smaller primary arrival, then a much
     * larger "echo" shortly after. STA/LTA must return the first crossing,
     * not the strongest one. */
    float samples[500] = { 0 };
    fill_background_sine(samples, 0u, 500u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 15u, 0.5f);
    insert_rect_pulse(samples, 260u, 15u, 2.0f);

    const geophone_sta_lta_config_t config = {
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .trigger_ratio = 3.0f
    };
    geophone_sta_lta_result_t result;

    assert(geophone_event_sta_lta_detect(samples, 500u, &config, &result));
    assert(result.found);
    /* Must land at the earlier (smaller) pulse, well before the echo at 260. */
    assert(result.trigger_index < 220u);
}


static void test_sta_lta_survives_a_cadence_gap_without_false_trigger(void)
{
    /* A spliced-out acquisition gap (exact zeros) between two quiet
     * background segments must not itself look like the event; the real,
     * much larger pulse afterwards must still be the one reported. */
    float samples[600] = { 0 };
    fill_background_sine(samples, 0u, 100u, 0.05f, 0.05f);
    /* samples[100..149] left at 0.0f: simulated dropped acquisition window. */
    fill_background_sine(samples, 150u, 150u, 0.05f, 0.05f);
    insert_rect_pulse(samples, 320u, 20u, 1.0f);

    const geophone_sta_lta_config_t config = {
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .trigger_ratio = 6.0f
    };
    geophone_sta_lta_result_t result;

    assert(geophone_event_sta_lta_detect(samples, 600u, &config, &result));
    assert(result.found);
    assert(result.trigger_index >= 300u && result.trigger_index < 330u);
}


static void test_sta_lta_rejects_invalid_windows(void)
{
    float samples[50] = { 0 };
    const geophone_sta_lta_config_t bad_order = {
        .sta_window_samples = 50u,
        .lta_window_samples = 10u,
        .trigger_ratio = 3.0f
    };
    geophone_sta_lta_result_t result;

    assert(!geophone_event_sta_lta_detect(samples, 50u, &bad_order, &result));
}


/* ---------------------------------------------------------------- */
/* First-arrival refinement                                          */
/* ---------------------------------------------------------------- */

static void test_refine_arrival_locates_step_onset(void)
{
    float samples[300] = { 0 };
    insert_rect_pulse(samples, 200u, 50u, 1.0f);

    const geophone_arrival_refine_config_t config = {
        .lookback_samples = 15u,
        .lookahead_samples = 15u,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t result;

    /* A coarse detector reporting a few samples early, as STA/LTA's window
     * start typically does. */
    assert(geophone_event_refine_arrival(samples, 300u, 191u, &config, &result));
    assert(result.found);
    assert(result.refined_index == 200u);
    assert(result.local_peak_abs == 1.0f);
}


static void test_refine_arrival_on_silent_window_is_not_found(void)
{
    float samples[300] = { 0 };
    const geophone_arrival_refine_config_t config = {
        .lookback_samples = 10u,
        .lookahead_samples = 10u,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t result;

    assert(!geophone_event_refine_arrival(samples, 300u, 150u, &config, &result));
    assert(!result.found);
}


/* ---------------------------------------------------------------- */
/* Pairwise cross-correlation delay refinement                       */
/* ---------------------------------------------------------------- */

static void test_cross_correlate_recovers_delay_with_amplitude_imbalance(void)
{
    float channel_a[400] = { 0 };
    float channel_b[400] = { 0 };

    insert_triangle_pulse(channel_a, 400u, 150u, 20u, 1.0f);
    insert_triangle_pulse(channel_b, 400u, 165u, 20u, 0.3f);

    const geophone_cross_correlation_config_t config = {
        .max_lag_samples = 50u
    };
    geophone_cross_correlation_result_t result;

    assert(geophone_event_cross_correlate(channel_a, channel_b, 400u, &config, &result));
    assert(result.found);
    assert(result.delay_samples == 15);
    assert(result.peak_correlation > 0.95f);
}


static void test_cross_correlate_with_missing_channel_is_not_found(void)
{
    float channel_a[200] = { 0 };
    float channel_b[200] = { 0 };
    insert_triangle_pulse(channel_a, 200u, 100u, 15u, 1.0f);
    /* channel_b stays all-zero: sensor disconnected / no data. */

    const geophone_cross_correlation_config_t config = { .max_lag_samples = 30u };
    geophone_cross_correlation_result_t result;

    assert(!geophone_event_cross_correlate(channel_a, channel_b, 200u, &config, &result));
    assert(!result.found);
}


static void test_cross_correlate_identical_signal_peaks_at_zero_lag(void)
{
    float channel_a[200] = { 0 };
    insert_triangle_pulse(channel_a, 200u, 100u, 15u, 1.0f);

    const geophone_cross_correlation_config_t config = { .max_lag_samples = 30u };
    geophone_cross_correlation_result_t result;

    assert(geophone_event_cross_correlate(channel_a, channel_a, 200u, &config, &result));
    assert(result.delay_samples == 0);
    assert(fabsf(result.peak_correlation - 1.0f) < 0.001f);
}


/* MAX-209: a lone early noise spike above the threshold must not become the
 * onset; the pick belongs to the burst that contains the peak. */
static void test_refine_arrival_ignores_isolated_early_spike(void)
{
    float samples[300] = { 0 };
    insert_rect_pulse(samples, 200u, 50u, 1.0f);
    samples[188] = 0.7f; /* spike 12 samples before the true onset */

    const geophone_arrival_refine_config_t config = {
        .lookback_samples = 15u,
        .lookahead_samples = 15u,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t result;
    assert(geophone_event_refine_arrival(samples, 300u, 191u, &config, &result));
    assert(result.refined_index == 200u);
}


/* MAX-209: a linear ramp crossing the threshold between samples resolves
 * to a sub-sample onset. */
static void test_refine_arrival_interpolates_sub_sample_crossing(void)
{
    float samples[300] = { 0 };
    /* 0, 0.2, 0.4, 0.6, 0.8, 1.0 ... from index 100: 0.5 lies at 102.5. */
    for (size_t i = 0u; i < 6u; ++i) {
        samples[100u + i] = 0.2f * (float)i;
    }
    for (size_t i = 106u; i < 140u; ++i) {
        samples[i] = 1.0f;
    }

    const geophone_arrival_refine_config_t config = {
        .lookback_samples = 10u,
        .lookahead_samples = 20u,
        .amplitude_fraction = 0.5f
    };
    geophone_arrival_refine_result_t result;
    assert(geophone_event_refine_arrival(samples, 300u, 100u, &config, &result));
    assert(result.refined_index == 103u);
    assert(fabsf(((float)result.refined_index + result.refined_fraction) - 102.5f) < 1e-4f);
}


/* MAX-209: parabolic interpolation recovers a fractional delay. */
static void test_cross_correlate_recovers_fractional_delay(void)
{
    float channel_a[400] = { 0 };
    float channel_b[400] = { 0 };
    /* Smooth Gaussian pulses 12.4 samples apart. */
    for (size_t i = 0u; i < 400u; ++i) {
        const double ta = ((double)i - 150.0) / 6.0;
        const double tb = ((double)i - 162.4) / 6.0;
        channel_a[i] = (float)exp(-0.5 * ta * ta);
        channel_b[i] = (float)(0.4 * exp(-0.5 * tb * tb));
    }

    const geophone_cross_correlation_config_t config = { .max_lag_samples = 40u };
    geophone_cross_correlation_result_t result;
    assert(geophone_event_cross_correlate(channel_a, channel_b, 400u, &config, &result));
    assert(result.delay_samples == 12);
    assert(fabs(result.delay_refined_samples - 12.4) < 0.1);
}


/* ---------------------------------------------------------------- */
/* SNR / accept-reject gate                                          */
/* ---------------------------------------------------------------- */

static void test_snr_of_clean_impulse_over_quiet_noise(void)
{
    float samples[300] = { 0 };
    fill_background_sine(samples, 0u, 50u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    const float snr = geophone_event_compute_snr(samples, 300u, 50u, 200u, 20u);
    assert(snr > 10.0f);
}


static void test_snr_degenerate_all_zero_is_zero_not_nan(void)
{
    float samples[300] = { 0 };
    const float snr = geophone_event_compute_snr(samples, 300u, 50u, 200u, 20u);
    assert(snr == 0.0f);
    assert(!isnan(snr));
}


static void test_pick_is_acceptable_gate(void)
{
    const geophone_pick_quality_config_t config = { .minimum_snr = 5.0f };
    assert(geophone_event_pick_is_acceptable(10.0f, &config));
    assert(!geophone_event_pick_is_acceptable(1.0f, &config));
    assert(geophone_event_pick_is_acceptable(5.0f, &config)); /* exactly at threshold */
}


/* ---------------------------------------------------------------- */
/* Combined per-channel pick                                         */
/* ---------------------------------------------------------------- */

static geophone_channel_pick_config_t default_pick_config(void)
{
    return (geophone_channel_pick_config_t) {
        .noise_window_samples = 50u,
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .sta_lta_trigger_ratio = 3.0f,
        .refine_lookback_samples = 15u,
        .refine_lookahead_samples = 15u,
        .refine_amplitude_fraction = 0.5f,
        .snr_signal_window_samples = 20u,
        .minimum_snr = 5.0f
    };
}


static void test_pick_channel_arrival_accepts_a_clean_event(void)
{
    float samples[300] = { 0 };
    fill_background_sine(samples, 0u, 300u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    const geophone_channel_pick_config_t config = default_pick_config();
    geophone_channel_pick_t result;

    assert(geophone_event_pick_channel_arrival(samples, 300u, 1000u, &config, &result));
    assert(result.status == GEOPHONE_PICK_ACCEPTED);
    assert(result.method == GEOPHONE_PICK_METHOD_STA_LTA_REFINED);
    assert(result.arrival_sample_index == 200u);
    assert(fabsf(result.arrival_time_s - 0.2f) < 0.001f);
    assert(result.snr > 5.0f);
}


static void test_pick_channel_arrival_rejects_missing_channel(void)
{
    float samples[300] = { 0 };
    const geophone_channel_pick_config_t config = default_pick_config();
    geophone_channel_pick_t result;

    assert(!geophone_event_pick_channel_arrival(samples, 300u, 1000u, &config, &result));
    assert(result.status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
}


static void test_pick_channel_arrival_rejects_low_snr_gate(void)
{
    float samples[300] = { 0 };
    fill_background_sine(samples, 0u, 300u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    geophone_channel_pick_config_t strict_config = default_pick_config();
    strict_config.minimum_snr = 1000.0f;
    geophone_channel_pick_t result;

    assert(!geophone_event_pick_channel_arrival(samples, 300u, 1000u, &strict_config, &result));
    assert(result.status == GEOPHONE_PICK_REJECTED_LOW_SNR);
    /* Diagnostic fields are still populated even though the pick was
     * rejected, so callers/logs can see *why*, not just that it failed. */
    assert(result.arrival_sample_index == 200u);
    assert(result.snr > 0.0f);
}


static void test_pick_channel_arrival_rejects_null_arguments(void)
{
    float samples[10] = { 0 };
    const geophone_channel_pick_config_t config = default_pick_config();
    geophone_channel_pick_t result;

    assert(!geophone_event_pick_channel_arrival(NULL, 10u, 1000u, &config, &result));
    assert(!geophone_event_pick_channel_arrival(samples, 10u, 1000u, NULL, &result));
}


static void test_pick_uncertainty_saturates_instead_of_wrapping(void)
{
    float samples[300] = { 0 };
    fill_background_sine(samples, 0u, 300u, 0.01f, 0.05f);
    insert_rect_pulse(samples, 200u, 20u, 1.0f);

    geophone_channel_pick_config_t config = default_pick_config();
    config.refine_lookback_samples = SIZE_MAX;
    config.refine_lookahead_samples = SIZE_MAX;
    geophone_channel_pick_t result;

    assert(geophone_event_pick_channel_arrival(
        samples, 300u, 1000u, &config, &result
    ));
    assert(result.uncertainty_samples == SIZE_MAX);
}


int main(void)
{
    test_refine_arrival_ignores_isolated_early_spike();
    test_refine_arrival_interpolates_sub_sample_crossing();
    test_cross_correlate_recovers_fractional_delay();
    test_energy_trigger_finds_impulse_after_quiet_baseline();
    test_energy_trigger_rejects_invalid_arguments();
    test_energy_trigger_never_fires_on_pure_silence();
    test_geophone_math_rejects_nonfinite_inputs();
    test_geophone_math_rejects_window_overflow_and_clamps_edges();

    test_sta_lta_finds_impulse_over_quiet_background();
    test_sta_lta_missing_channel_never_triggers();
    test_sta_lta_picks_earliest_arrival_not_the_larger_echo();
    test_sta_lta_survives_a_cadence_gap_without_false_trigger();
    test_sta_lta_rejects_invalid_windows();

    test_refine_arrival_locates_step_onset();
    test_refine_arrival_on_silent_window_is_not_found();

    test_cross_correlate_recovers_delay_with_amplitude_imbalance();
    test_cross_correlate_with_missing_channel_is_not_found();
    test_cross_correlate_identical_signal_peaks_at_zero_lag();

    test_snr_of_clean_impulse_over_quiet_noise();
    test_snr_degenerate_all_zero_is_zero_not_nan();
    test_pick_is_acceptable_gate();

    test_pick_channel_arrival_accepts_a_clean_event();
    test_pick_channel_arrival_rejects_missing_channel();
    test_pick_channel_arrival_rejects_low_snr_gate();
    test_pick_channel_arrival_rejects_null_arguments();
    test_pick_uncertainty_saturates_instead_of_wrapping();

    puts("geophone_events host tests passed");
    return 0;
}
