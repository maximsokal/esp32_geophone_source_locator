#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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


static uint32_t lcg_next(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}


/* Uniform white sample in [-0.5, 0.5). */
static float lcg_uniform(uint32_t *state)
{
    return (float)(lcg_next(state) >> 8) / (float)(1u << 24) - 0.5f;
}


/* ---------------------------------------------------------------- */
/* Colored-noise backgrounds (MAX-134 acceptance gap)                */
/*                                                                    */
/* Two deterministic, seeded generators, neither flat-spectrum white  */
/* noise nor a single pure tone:                                      */
/*  - brown/red noise: a leaky integrator driven by LCG white noise,  */
/*    concentrating energy at low frequencies like real ground        */
/*    microseism/traffic background;                                  */
/*  - band-limited noise: LCG white floor plus a slow sinusoidal       */
/*    drift and a fixed-frequency "mains hum"-like tone, i.e. energy   */
/*    concentrated at a few discrete bands rather than spread flat.    */
/* ---------------------------------------------------------------- */

/* Brown/red noise: y[n] = leak*y[n-1] + white[n], scaled by amplitude.
 * leak close to 1.0 concentrates more energy at low frequency. */
static void synth_brown_noise(
    float *buffer,
    size_t count,
    uint32_t seed,
    float amplitude,
    float leak
)
{
    uint32_t state = seed;
    float y = 0.0f;
    for (size_t i = 0u; i < count; ++i) {
        y = leak * y + lcg_uniform(&state);
        buffer[i] = amplitude * y;
    }
}


/* Band-limited colored noise: white floor + slow drift + a fixed-frequency
 * hum-like tone, all deterministic from `seed`. */
static void synth_band_limited_noise(
    float *buffer,
    size_t count,
    uint32_t seed,
    float white_amplitude,
    float drift_amplitude,
    float drift_cycles_per_sample,
    float hum_amplitude,
    float hum_cycles_per_sample
)
{
    uint32_t state = seed;
    for (size_t i = 0u; i < count; ++i) {
        buffer[i] =
            white_amplitude * lcg_uniform(&state) +
            drift_amplitude * sinf(TWO_PI * drift_cycles_per_sample * (float)i) +
            hum_amplitude * sinf(TWO_PI * hum_cycles_per_sample * (float)i);
    }
}


/* Shadow of geophone_event_sta_lta_detect()'s scan that returns the maximum
 * ratio observed over the whole buffer instead of stopping at first trigger,
 * so a "background never gets close to threshold" claim has a concrete
 * number behind it rather than just a single found/not-found bit. */
static float max_sta_lta_ratio(
    const float *samples,
    size_t sample_count,
    size_t sta_w,
    size_t lta_w
)
{
    double lta_sum = 0.0;
    for (size_t k = 0u; k < lta_w; ++k) {
        lta_sum += (double)samples[k] * (double)samples[k];
    }
    double sta_sum = 0.0;
    for (size_t k = lta_w - sta_w; k < lta_w; ++k) {
        sta_sum += (double)samples[k] * (double)samples[k];
    }

    float max_ratio = 0.0f;
    for (size_t end = lta_w; ; ++end) {
        const double lta_mean = lta_sum / (double)lta_w;
        if (lta_mean > 0.0) {
            const double ratio = (sta_sum / (double)sta_w) / lta_mean;
            if ((float)ratio > max_ratio) {
                max_ratio = (float)ratio;
            }
        }
        if (end >= sample_count) {
            break;
        }
        const double incoming = (double)samples[end] * (double)samples[end];
        lta_sum += incoming - (double)samples[end - lta_w] * (double)samples[end - lta_w];
        sta_sum += incoming - (double)samples[end - sta_w] * (double)samples[end - sta_w];
    }
    return max_ratio;
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


/* ---------------------------------------------------------------- */
/* Colored-noise backgrounds (MAX-134 acceptance gap)                */
/*                                                                    */
/* All parameters below (leak, amplitudes, frequencies, seeds) were   */
/* chosen empirically so the colored background alone stays well      */
/* below the trigger_ratio used in each test, then locked in as fixed */
/* literals; every test here is deterministic byte-for-byte.          */
/* ---------------------------------------------------------------- */

static geophone_sta_lta_config_t typical_sta_lta_config(void)
{
    return (geophone_sta_lta_config_t) {
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .trigger_ratio = 3.0f
    };
}


/* Brown/red noise (leaky-integrated white noise, leak = 0.3) alone, with no
 * inserted event: max_sta_lta_ratio() (a shadow of the production scan that
 * reports the worst ratio seen anywhere in the buffer, not just whether it
 * crossed the threshold) stays under the typical trigger_ratio = 3.0 used
 * throughout this file, and the real detector never fires. leak = 0.3 was
 * swept against 8 seeds and never exceeded ratio 2.6; this seed's actual
 * peak ratio is well inside that. */
static void test_brown_noise_alone_never_triggers_sta_lta(void)
{
    float samples[2000];
    synth_brown_noise(samples, 2000u, 12345u, 0.05f, 0.3f);

    const geophone_sta_lta_config_t config = typical_sta_lta_config();
    geophone_sta_lta_result_t result;

    const float max_ratio = max_sta_lta_ratio(samples, 2000u, config.sta_window_samples, config.lta_window_samples);
    assert(max_ratio < config.trigger_ratio);

    assert(!geophone_event_sta_lta_detect(samples, 2000u, &config, &result));
    assert(!result.found);
}


/* Band-limited colored noise (white floor + slow drift + a fixed-frequency
 * hum-like tone) alone stays below the same threshold. Different generator,
 * different spectral shape than the brown-noise case above, covering both
 * colored-noise variants the ticket calls out. */
static void test_band_limited_noise_alone_never_triggers_sta_lta(void)
{
    float samples[2000];
    synth_band_limited_noise(samples, 2000u, 42u, 0.02f, 0.01f, 0.0003f, 0.015f, 0.1f);

    const geophone_sta_lta_config_t config = typical_sta_lta_config();
    geophone_sta_lta_result_t result;

    const float max_ratio = max_sta_lta_ratio(samples, 2000u, config.sta_window_samples, config.lta_window_samples);
    assert(max_ratio < config.trigger_ratio);

    assert(!geophone_event_sta_lta_detect(samples, 2000u, &config, &result));
    assert(!result.found);
}


/* A known step onset (rect pulse, sample_rate = 1000 Hz) dropped into the
 * band-limited background is picked within +/-3 samples (3 ms) of the true
 * onset at index 1000. The tolerance is not 0: the coarse STA/LTA trigger
 * index and the refine search window it seeds can shift by a couple of
 * samples depending on the phase of the background's drift/hum components
 * at the moment the event arrives, since refine's local-peak search sees
 * that background too. Verified against the production pipeline
 * (geophone_event_pick_channel_arrival): this exact seed/config lands
 * exactly on index 1000, so +/-3 leaves headroom for other seeds/phases
 * without weakening the assertion to "somewhere nearby". */
static void test_colored_noise_impulse_detected_within_tolerance(void)
{
    float samples[2000];
    synth_band_limited_noise(samples, 2000u, 777u, 0.02f, 0.01f, 0.0003f, 0.015f, 0.1f);

    const size_t truth_index = 1000u;
    insert_rect_pulse(samples, truth_index, 30u, 1.0f);

    const geophone_channel_pick_config_t config = {
        .noise_window_samples = 500u,
        .sta_window_samples = 10u,
        .lta_window_samples = 100u,
        .sta_lta_trigger_ratio = 3.0f,
        .refine_lookback_samples = 15u,
        .refine_lookahead_samples = 15u,
        .refine_amplitude_fraction = 0.5f,
        .snr_signal_window_samples = 20u,
        .minimum_snr = 3.0f
    };
    geophone_channel_pick_t result;

    assert(geophone_event_pick_channel_arrival(samples, 2000u, 1000u, &config, &result));
    assert(result.status == GEOPHONE_PICK_ACCEPTED);

    const long tolerance_samples = 3;
    const long delta =
        (long)result.arrival_sample_index - (long)truth_index;
    assert(delta >= -tolerance_samples && delta <= tolerance_samples);
    assert(fabsf(result.arrival_time_s - (float)truth_index / 1000.0f) < 0.005f);
}


/* Same band-limited colored background (seed 555): a strong event's SNR
 * clears a typical minimum_snr = 5.0 gate; a weak event on the identical
 * background does not. Actual values observed: strong ~77.9, weak ~1.56 --
 * asserted with margin below/above the concrete numbers so the test is not
 * pinned to float bit-exactness while still pinning down the SNR gate's
 * accept/reject boundary on colored (not white/silent) background. */
static void test_snr_gate_accepts_strong_rejects_weak_on_colored_background(void)
{
    float background[2000];
    synth_band_limited_noise(background, 2000u, 555u, 0.02f, 0.01f, 0.0003f, 0.015f, 0.1f);

    float strong[2000];
    float weak[2000];
    memcpy(strong, background, sizeof(strong));
    memcpy(weak, background, sizeof(weak));

    insert_triangle_pulse(strong, 2000u, 700u, 20u, 1.0f);
    insert_triangle_pulse(weak, 2000u, 700u, 20u, 0.02f);

    const float snr_strong = geophone_event_compute_snr(strong, 2000u, 500u, 700u, 20u);
    const float snr_weak = geophone_event_compute_snr(weak, 2000u, 500u, 700u, 20u);

    assert(snr_strong > 50.0f);
    assert(snr_weak > 0.0f && snr_weak < 5.0f);

    const geophone_pick_quality_config_t quality_config = { .minimum_snr = 5.0f };
    assert(geophone_event_pick_is_acceptable(snr_strong, &quality_config));
    assert(!geophone_event_pick_is_acceptable(snr_weak, &quality_config));
}


/* Two channels sharing the exact same brown-noise realization (as if both
 * geophones sit in the same ambient field) plus the same triangular event,
 * delayed by a known 15 samples on channel_b. Cross-correlation recovers
 * that delay (within +/-1 sample -- insert_triangle_pulse() overwrites the
 * background under the event rather than adding to it, which slightly
 * perturbs the correlation peak versus a noise-free pulse) even though the
 * "noise" is heavily autocorrelated in time (unlike flat white noise),
 * because the delay estimate comes from where the *shared* signal (noise +
 * event) lines up best, not from the event's amplitude alone. Actual
 * observed: delay_samples=14, delay_refined_samples~=14.34, peak
 * correlation~=0.91. */
static void test_cross_correlate_recovers_delay_on_shared_colored_noise(void)
{
    float background[1000];
    synth_brown_noise(background, 1000u, 999u, 0.05f, 0.95f);

    float channel_a[1000];
    float channel_b[1000];
    memcpy(channel_a, background, sizeof(channel_a));
    memcpy(channel_b, background, sizeof(channel_b));

    const int32_t known_delay = 15;
    insert_triangle_pulse(channel_a, 1000u, 200u, 20u, 1.0f);
    insert_triangle_pulse(channel_b, 1000u, 200u + (size_t)known_delay, 20u, 1.0f);

    const geophone_cross_correlation_config_t config = { .max_lag_samples = 50u };
    geophone_cross_correlation_result_t result;

    assert(geophone_event_cross_correlate(channel_a, channel_b, 1000u, &config, &result));
    assert(result.found);
    assert(labs((long)result.delay_samples - (long)known_delay) <= 1);
    assert(fabs(result.delay_refined_samples - (double)known_delay) < 1.0);
    assert(result.peak_correlation > 0.9f);
}


/* Boundary: an event fully inside the leading noise_window/LTA-fill region
 * (before the detector can even start scanning) never becomes a trigger --
 * it can only ever bias the initial LTA sum, since geophone_event_sta_lta_detect()
 * does not evaluate any ratio until sample lta_window_samples. This is the
 * documented, existing contract (see geophone_event_sta_lta.c), not a new
 * requirement; the colored background here would not trigger on its own
 * either (per test_band_limited_noise_alone_never_triggers_sta_lta), so a
 * detection here would only be possible via the too-early event, and the
 * contract says that can't happen. */
static void test_colored_noise_boundary_event_at_start_is_never_found(void)
{
    float samples[300];
    synth_band_limited_noise(samples, 300u, 111u, 0.02f, 0.01f, 0.0003f, 0.015f, 0.1f);
    insert_triangle_pulse(samples, 300u, 3u, 3u, 1.0f);

    const geophone_sta_lta_config_t config = typical_sta_lta_config();
    geophone_sta_lta_result_t result;

    assert(!geophone_event_sta_lta_detect(samples, 300u, &config, &result));
    assert(!result.found);
}


/* Boundary: an event whose onset lies inside the final sta_window_samples of
 * the buffer (no full trailing window remains after it) is still found --
 * the scan's last iteration evaluates the window ending exactly at
 * sample_count, so detection does not require room after the event. Trigger
 * index must land in [lta_w - sta_w, sample_count - sta_w] = [90, 290] per
 * the documented contract; the actual value (286) lands well inside, close
 * to the pulse's true onset at 295. */
static void test_colored_noise_boundary_event_at_end_is_found(void)
{
    float samples[300];
    synth_band_limited_noise(samples, 300u, 222u, 0.02f, 0.01f, 0.0003f, 0.015f, 0.1f);
    insert_rect_pulse(samples, 295u, 5u, 1.0f);

    const geophone_sta_lta_config_t config = typical_sta_lta_config();
    geophone_sta_lta_result_t result;

    assert(geophone_event_sta_lta_detect(samples, 300u, &config, &result));
    assert(result.found);
    assert(result.trigger_index >= 90u && result.trigger_index <= 290u);
    assert(result.trigger_index >= 280u && result.trigger_index <= 290u);
}


/* ---- Secondary (S-wave) pick for PSAFUVS -------------------------------- */

/* Deterministic background noise, a P wavelet at p_index with a decaying
 * coda, and a larger, lower-frequency S wavelet at s_index (0 = none). */
static void synth_p_s_trace(float *buffer, size_t count, size_t p_index, size_t s_index, float s_amplitude)
{
    uint32_t state = 7u;
    for (size_t i = 0u; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        buffer[i] = 0.02f * ((float)(state >> 8) / (float)(1u << 24) - 0.5f);
        if (i >= p_index) {
            const float t = (float)(i - p_index);
            buffer[i] += 1.0f * expf(-t / 60.0f) * sinf(TWO_PI * 0.08f * t);
        }
        if (s_index > 0u && i >= s_index) {
            const float t = (float)(i - s_index);
            buffer[i] += s_amplitude * expf(-t / 90.0f) * sinf(TWO_PI * 0.03f * t);
        }
    }
}


static geophone_secondary_pick_config_t secondary_config(void)
{
    return (geophone_secondary_pick_config_t) {
        .min_separation_samples = 20u,
        .max_separation_samples = 400u,
        .sta_window_samples = 10u,
        .lta_window_samples = 60u,
        .sta_lta_trigger_ratio = 3.0f,
        .minimum_snr = 2.0f,
    };
}


static void test_secondary_pick_finds_the_s_onset_after_p(void)
{
    float trace[1000];
    synth_p_s_trace(trace, 1000u, 200u, 350u, 3.0f);
    const geophone_secondary_pick_config_t config = secondary_config();
    geophone_channel_pick_t pick;
    assert(geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    assert(pick.status == GEOPHONE_PICK_ACCEPTED);
    assert(pick.arrival_sample_index >= 347u && pick.arrival_sample_index <= 353u);
    assert(fabsf(pick.arrival_time_s - (float)pick.arrival_sample_index / 500.0f) < 1.0e-6f);
    assert(pick.snr >= 2.0f);
    assert(pick.uncertainty_samples == 10u);
    assert(pick.arrival_subsample_offset == 0.0f);
}


static void test_secondary_pick_never_triggers_on_the_p_arrival_itself(void)
{
    /* No S wave: the P onset and its coda alone must not produce a pick. */
    float trace[1000];
    synth_p_s_trace(trace, 1000u, 200u, 0u, 0.0f);
    const geophone_secondary_pick_config_t config = secondary_config();
    geophone_channel_pick_t pick;
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    assert(pick.status != GEOPHONE_PICK_ACCEPTED);
}


static void test_secondary_pick_respects_the_search_window_and_snr(void)
{
    float trace[1000];
    synth_p_s_trace(trace, 1000u, 200u, 700u, 3.0f);
    geophone_secondary_pick_config_t config = secondary_config();
    geophone_channel_pick_t pick;
    /* S at P + 500 lies outside [P + 20, P + 400]. */
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    assert(pick.status == GEOPHONE_PICK_REJECTED_NO_TRIGGER);
    config.max_separation_samples = 600u;
    assert(geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));

    /* A weak S barely above the coda fails an unreachable SNR gate. */
    synth_p_s_trace(trace, 1000u, 200u, 350u, 3.0f);
    config = secondary_config();
    config.minimum_snr = 1000.0f;
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    assert(pick.status == GEOPHONE_PICK_REJECTED_LOW_SNR);
    assert(pick.arrival_sample_index > 200u);
}


static void test_secondary_pick_rejects_invalid_arguments(void)
{
    float trace[1000];
    synth_p_s_trace(trace, 1000u, 200u, 350u, 3.0f);
    geophone_secondary_pick_config_t config = secondary_config();
    geophone_channel_pick_t pick;
    assert(!geophone_event_pick_secondary_arrival(NULL, 1000u, 500u, 200u, &config, &pick));
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, NULL, &pick));
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, NULL));
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 1000u, &config, &pick));
    config.lta_window_samples = config.sta_window_samples;
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    config = secondary_config();
    config.max_separation_samples = config.min_separation_samples;
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    config = secondary_config();
    trace[380] = NAN;
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 200u, &config, &pick));
    /* A P pick too close to the end leaves no room for the windows. */
    synth_p_s_trace(trace, 1000u, 200u, 350u, 3.0f);
    assert(!geophone_event_pick_secondary_arrival(trace, 1000u, 500u, 995u, &config, &pick));
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

    test_brown_noise_alone_never_triggers_sta_lta();
    test_band_limited_noise_alone_never_triggers_sta_lta();
    test_colored_noise_impulse_detected_within_tolerance();
    test_snr_gate_accepts_strong_rejects_weak_on_colored_background();
    test_cross_correlate_recovers_delay_on_shared_colored_noise();
    test_colored_noise_boundary_event_at_start_is_never_found();
    test_colored_noise_boundary_event_at_end_is_found();

    test_secondary_pick_finds_the_s_onset_after_p();
    test_secondary_pick_never_triggers_on_the_p_arrival_itself();
    test_secondary_pick_respects_the_search_window_and_snr();
    test_secondary_pick_rejects_invalid_arguments();

    puts("geophone_events host tests passed");
    return 0;
}
