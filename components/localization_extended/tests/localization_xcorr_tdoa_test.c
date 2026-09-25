#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ext_test_support.h"
#include "localization_robust.h"
#include "localization_xcorr_tdoa.h"

/*
 * Network cross-correlation TDOA on synthetic geophone records sampled at
 * the project's ~500 Hz: the delays come from whole waveforms, so onset
 * picks jittered by a couple of samples (about a metre of travel at
 * 300 m/s) do not move the source; a pair that locks onto the wrong cycle
 * is caught by the closure of the pair network.
 */

#define FS 500U
#define V_MPS 300.0
#define N_SAMPLES 1000U
#define RICKER_HZ 30.0
#define EVENT_S 0.5

static float g_samples[8][N_SAMPLES];


static double ricker(double t)
{
    const double a = 3.141592653589793 * RICKER_HZ * t;
    return (1.0 - 2.0 * a * a) * exp(-a * a);
}


/* Records with 1/D amplitude, white noise of `noise` x the peak, and
 * onset picks jittered by +-jitter samples. */
static void make_channels(
    const localization_point3d_t *sensors,
    size_t count,
    localization_point3d_t source,
    double noise,
    double jitter,
    uint32_t *state,
    localization_xcorr_channel_t *out,
    localization_observation_t *picks
)
{
    for (size_t i = 0U; i < count; ++i) {
        const double d = ext_test_distance(source, sensors[i]);
        const double arrival_s = EVENT_S + d / V_MPS;
        /* Onset of the zero-phase Ricker: one period before its peak. */
        const double onset_s = arrival_s - 1.0 / RICKER_HZ;
        for (size_t n = 0U; n < N_SAMPLES; ++n) {
            g_samples[i][n] = (float)(ricker((double)n / FS - arrival_s) / (0.5 + d) +
                                      noise * ext_test_normal(state));
        }
        out[i].samples = g_samples[i];
        out[i].sample_count = N_SAMPLES;
        out[i].sample_rate_hz = FS;
        out[i].sensor_position = sensors[i];
        out[i].pick_valid = true;
        out[i].pick_sample = onset_s * FS + ext_test_uniform(state, jitter);
        if (picks != NULL) {
            picks[i].sensor_position = sensors[i];
            picks[i].arrival_time_us = (int64_t)llround(out[i].pick_sample / FS * 1.0e6);
        }
    }
}


static localization_xcorr_tdoa_config_t config(void)
{
    localization_xcorr_tdoa_config_t c;
    memset(&c, 0, sizeof(c));
    c.propagation_speed_mps = V_MPS;
    c.timing_uncertainty_us = 1000.0;  /* half a sample at c = 0.7 */
    c.window_samples = 40U;
    c.max_lag_samples = 10U;
    return c;
}


static const localization_extended_bounds_t PLANE = { { -1.0, -1.0, 0.0 }, { 4.0, 3.5, 0.0 }, 0.05 };


static void test_waveform_delays_beat_jittered_picks(void)
{
    const localization_point3d_t source = { 1.1, 1.6, 0.0 };
    uint32_t state = 7U;
    localization_xcorr_channel_t channels[4];
    localization_observation_t picks[4];
    make_channels(EXT_TEST_SENSORS_FLAT, 4U, source, 0.01, 2.0, &state, channels, picks);
    const localization_xcorr_tdoa_config_t c = config();

    localization_xcorr_tdoa_result_t result;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_XCORR_TDOA_SOLVED);
    assert(result.planar);
    assert(result.pairs_total == 6U && result.pairs_measured == 6U && result.pairs_used == 6U);
    assert(result.pair_outlier_mask == 0U && result.channel_mask_used == 0xFU);
    assert(result.degrees_of_freedom == 1);
    assert(result.mean_correlation > 0.9);
    const double xcorr_error = ext_test_distance(result.position, source);

    /* The same event from the jittered onset picks (least squares). */
    localization_robust_config_t ls = localization_robust_default_config();
    ls.propagation_speed_mps = V_MPS;
    ls.timing_uncertainty_us = 400.0;
    ls.consensus = false;
    ls.loss = LOCALIZATION_EXTENDED_LOSS_HUBER;
    ls.tuning_k = 1.0e9;
    localization_robust_result_t from_picks;
    assert(localization_robust_solve(picks, 4U, &PLANE, &ls, &from_picks) == ESP_OK);
    const double pick_error = ext_test_distance(from_picks.position, source);
    printf("  +-2 sample pick jitter: picks %.3f m, network xcorr %.3f m (closure %.0f us)\n",
           pick_error, xcorr_error, result.closure_rms_us);
    assert(xcorr_error < 0.1);
    assert(pick_error > 3.0 * xcorr_error);

    /* Network-adjusted delays behind the earliest channel. */
    double nearest = INFINITY;
    for (size_t i = 0U; i < 4U; ++i) {
        nearest = fmin(nearest, ext_test_distance(source, EXT_TEST_SENSORS_FLAT[i]));
    }
    for (size_t i = 0U; i < 4U; ++i) {
        const double expected_us = (ext_test_distance(source, EXT_TEST_SENSORS_FLAT[i]) - nearest) / V_MPS * 1.0e6;
        /* 0.1 sample: the white noise's own share. */
        assert(fabs(result.relative_delay_us[i] - expected_us) < 200.0);
    }
}


static void test_buried_source_under_relief(void)
{
    const localization_point3d_t source = { 1.7, 1.4, -1.2 };
    uint32_t state = 12U;
    localization_xcorr_channel_t channels[6];
    make_channels(EXT_TEST_SENSORS_3D, 6U, source, 0.01, 2.0, &state, channels, NULL);
    const localization_xcorr_tdoa_config_t c = config();
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_xcorr_tdoa_result_t result;
    assert(localization_xcorr_tdoa_solve(channels, 6U, &bounds, &c, &result) == ESP_OK);
    printf("  6 sensors with relief: error %.3f m, %zu/%zu pairs\n",
           ext_test_distance(result.position, source), result.pairs_used, result.pairs_total);
    assert(result.status == LOCALIZATION_XCORR_TDOA_SOLVED);
    assert(!result.planar && result.pairs_total == 15U);
    assert(result.degrees_of_freedom == 2);
    assert(ext_test_distance(result.position, source) < 0.25);
}


static void test_cycle_skipped_pair_is_caught_by_closure(void)
{
    const localization_point3d_t source = { 1.1, 1.6, 0.0 };
    uint32_t state = 7U;
    localization_xcorr_channel_t channels[4];
    make_channels(EXT_TEST_SENSORS_FLAT, 4U, source, 0.01, 0.0, &state, channels, NULL);
    const localization_xcorr_tdoa_config_t c = config();
    localization_xcorr_pair_t pairs[LOCALIZATION_XCORR_MAX_PAIRS];
    size_t count = 0U;
    size_t total = 0U;
    assert(localization_xcorr_measure_pairs(channels, 4U, &c, pairs, &count, &total) == ESP_OK);
    assert(count == 6U && total == 6U);
    assert(pairs[0].first == 0U && pairs[0].second == 1U && pairs[5].first == 2U && pairs[5].second == 3U);

    localization_xcorr_tdoa_result_t clean;
    assert(localization_xcorr_tdoa_locate(channels, 4U, pairs, count, &PLANE, &c, &clean) == ESP_OK);

    /* Pair (1, 3) locks one full wavelet period late. */
    pairs[4].delay_s += 1.0 / RICKER_HZ;
    localization_xcorr_tdoa_result_t skipped;
    assert(localization_xcorr_tdoa_locate(channels, 4U, pairs, count, &PLANE, &c, &skipped) == ESP_OK);
    printf("  cycle skip on pair (1,3): clean %.3f m, with skip %.3f m, flagged mask 0x%x\n",
           ext_test_distance(clean.position, source), ext_test_distance(skipped.position, source),
           (unsigned)skipped.pair_outlier_mask);
    assert(skipped.status == LOCALIZATION_XCORR_TDOA_SOLVED);
    assert(skipped.pair_outlier_mask == (1U << 4));
    assert(skipped.pairs_used == 5U && skipped.channel_mask_used == 0xFU);
    assert(ext_test_distance(skipped.position, source) < 0.15);
    assert(skipped.closure_rms_us < 300.0);
}


static void test_uncorrelated_channel_drops_out(void)
{
    const localization_point3d_t source = { 1.1, 1.6, 0.0 };
    uint32_t state = 31U;
    localization_xcorr_channel_t channels[4];
    make_channels(EXT_TEST_SENSORS_FLAT, 4U, source, 0.01, 1.0, &state, channels, NULL);
    for (size_t n = 0U; n < N_SAMPLES; ++n) {
        g_samples[3][n] = (float)ext_test_normal(&state); /* a dead / noisy pod */
    }
    const localization_xcorr_tdoa_config_t c = config();
    localization_xcorr_tdoa_result_t result;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_OK);
    assert(result.pairs_measured == 3U);
    assert(result.channel_mask_used == 0x7U && result.channels_used == 3U);
    assert(result.degrees_of_freedom == 0);
    assert(result.status == LOCALIZATION_XCORR_TDOA_SOLVED);
    assert(ext_test_distance(result.position, source) < 0.15);
}


static void test_unanchored_and_invalid(void)
{
    const localization_point3d_t source = { 1.1, 1.6, 0.0 };
    uint32_t state = 3U;
    localization_xcorr_channel_t channels[4];
    make_channels(EXT_TEST_SENSORS_FLAT, 4U, source, 0.01, 0.0, &state, channels, NULL);
    localization_xcorr_tdoa_config_t c = config();
    localization_xcorr_tdoa_result_t result;

    /* One pick is enough to anchor every pair with that channel. */
    for (size_t i = 1U; i < 4U; ++i) {
        channels[i].pick_valid = false;
    }
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_OK);
    assert(result.pairs_measured == 3U);
    channels[0].pick_valid = false;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_XCORR_TDOA_NO_ANCHOR);
    for (size_t i = 0U; i < 4U; ++i) {
        channels[i].pick_valid = true;
    }

    channels[2].sample_rate_hz = 1000U;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_ERR_INVALID_ARG);
    channels[2].sample_rate_hz = FS;
    assert(localization_xcorr_tdoa_solve(channels, 1U, &PLANE, &c, &result) == ESP_ERR_INVALID_ARG);
    c.window_samples = 2U;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_ERR_INVALID_ARG);
    c = config();
    c.min_correlation = 1.0;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_ERR_INVALID_ARG);
    c = config();
    localization_xcorr_pair_t bad = { 0U, 0U, 0.0, 0.9, 1e-4 };
    assert(localization_xcorr_tdoa_locate(channels, 4U, &bad, 1U, &PLANE, &c, &result) == ESP_ERR_INVALID_ARG);

    /* A pick gate narrower than the true delay finds nothing to accept. */
    c.max_pick_disagreement_samples = 0.5;
    channels[1].pick_sample += 6.0;
    assert(localization_xcorr_tdoa_solve(channels, 4U, &PLANE, &c, &result) == ESP_OK);
    assert(result.pairs_measured < 6U);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_waveform_delays_beat_jittered_picks();
    test_buried_source_under_relief();
    test_cycle_skipped_pair_is_caught_by_closure();
    test_uncorrelated_channel_drops_out();
    test_unanchored_and_invalid();
    printf("localization xcorr TDOA tests passed\n");
    return 0;
}
