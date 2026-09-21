#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "seismic_dsp.h"


#define EPSILON 0.0001f
#define TEST_TWO_PI 6.28318530717958647692f

static bool nearly_equal(float a, float b)
{
    return fabsf(a - b) < EPSILON;
}

static float rms_of(const float *input, size_t sample_count)
{
    double sum_squares = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        sum_squares += (double)input[i] * (double)input[i];
    }
    return (float)sqrt(sum_squares / (double)sample_count);
}


_Static_assert(
    SEISMIC_DSP_FIXED_RATE_REQUIRES_CADENCE_VALID_INPUT == 1u,
    "fixed-rate DSP must require cadence-valid input"
);


static void test_requires_initialization(void)
{
    seismic_dsp_result_t result;
    float sample = 1.0f;

    assert(seismic_dsp_process_block(&sample, 1u, false, &result) == ESP_ERR_INVALID_STATE);
}


static void test_rejects_invalid_arguments(void)
{
    float sample = 1.0f;
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(NULL) == ESP_OK);
    assert(seismic_dsp_process_block(&sample, 1u, true, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 1u, true, &result) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 0u, true, &result) == ESP_OK);
    assert(result.completed_stages == 0u);
    assert(result.input_passthrough);
}


static void test_rejects_unknown_stage_bits(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_ALL | (1u << 31)
    };

    assert(seismic_dsp_init(&config) == ESP_ERR_INVALID_ARG);
}


static void test_bandpass_requires_valid_corners(void)
{
    const seismic_dsp_config_t missing_rate = {
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f }
    };
    const seismic_dsp_config_t inverted_corners = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 150.0f, .high_cutoff_hz = 50.0f }
    };
    const seismic_dsp_config_t above_nyquist = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 600.0f }
    };
    const seismic_dsp_config_t zero_low = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 0.0f, .high_cutoff_hz = 150.0f }
    };
    const seismic_dsp_config_t high_at_nyquist = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 500.0f }
    };

    assert(seismic_dsp_init(&missing_rate) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&inverted_corners) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&above_nyquist) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&zero_low) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&high_at_nyquist) == ESP_ERR_INVALID_ARG);
}


static void test_notch_requires_valid_parameters(void)
{
    const seismic_dsp_config_t missing_rate = {
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 100.0f, .q_factor = 10.0f }
    };
    const seismic_dsp_config_t zero_q = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 100.0f, .q_factor = 0.0f }
    };
    const seismic_dsp_config_t zero_center = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 0.0f, .q_factor = 10.0f }
    };
    const seismic_dsp_config_t center_at_nyquist = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 500.0f, .q_factor = 10.0f }
    };
    const seismic_dsp_config_t above_nyquist = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 900.0f, .q_factor = 10.0f }
    };

    assert(seismic_dsp_init(&missing_rate) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&zero_q) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&zero_center) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&center_at_nyquist) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_init(&above_nyquist) == ESP_ERR_INVALID_ARG);
}


static void test_empty_block_leaves_requested_stages_pending(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages =
            SEISMIC_DSP_STAGE_DC_REMOVAL |
            SEISMIC_DSP_STAGE_BANDPASS |
            SEISMIC_DSP_STAGE_NOTCH |
            SEISMIC_DSP_STAGE_ENERGY_RMS |
            SEISMIC_DSP_STAGE_NORMALIZATION |
            SEISMIC_DSP_STAGE_WINDOWING |
            SEISMIC_DSP_STAGE_FFT,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f },
        .notch = { .center_hz = 100.0f, .q_factor = 10.0f }
    };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(NULL, 0u, false, &result) == ESP_OK);
    assert(result.sample_count == 0u);
    assert(result.completed_stages == 0u);
    assert(result.placeholder_stages == config.requested_stages);
    assert(result.input_passthrough);
    assert(result.cadence_reset_applied);
    assert(result.energy_rms == 0.0f);
    assert(result.peak_abs == 0.0f);
    assert(result.fft_dominant_frequency_hz == 0.0f);
    assert(result.fft_dominant_magnitude == 0.0f);
}


static void test_stateless_gap_does_not_claim_filter_reset(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL
    };
    float samples[] = { 1.0f, 2.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 2u, false, &result) == ESP_OK);
    assert(!result.cadence_reset_applied);
}


static void test_unrequested_stages_stay_placeholder_and_passthrough(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 500u,
        .requested_stages = 0u
    };
    float samples[] = { -1.0f, 0.0f, 1.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    assert(result.sample_count == 3u);
    assert(result.sample_rate_hz == 500u);
    assert(result.requested_stages == 0u);
    assert(result.placeholder_stages == 0u);
    assert(result.completed_stages == 0u);
    assert(result.input_passthrough);
    assert(samples[0] == -1.0f && samples[1] == 0.0f && samples[2] == 1.0f);
}


static void test_unimplemented_stages_remain_placeholders(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 500u,
        .requested_stages = SEISMIC_DSP_STAGE_ALL,
        .bandpass = { .low_cutoff_hz = 10.0f, .high_cutoff_hz = 200.0f },
        .notch = { .center_hz = 50.0f, .q_factor = 10.0f }
    };
    float samples[] = { -1.0f, 0.0f, 1.0f };
    seismic_dsp_result_t result;

    const uint32_t implemented =
        SEISMIC_DSP_STAGE_DC_REMOVAL |
        SEISMIC_DSP_STAGE_BANDPASS |
        SEISMIC_DSP_STAGE_NOTCH |
        SEISMIC_DSP_STAGE_ENERGY_RMS |
        SEISMIC_DSP_STAGE_NORMALIZATION |
        SEISMIC_DSP_STAGE_WINDOWING |
        SEISMIC_DSP_STAGE_FFT;
    const uint32_t still_pending =
        SEISMIC_DSP_STAGE_FIR |
        SEISMIC_DSP_STAGE_IIR |
        SEISMIC_DSP_STAGE_CROSS_CORRELATION;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, false, &result) == ESP_OK);
    assert(result.completed_stages == implemented);
    assert(result.placeholder_stages == still_pending);
    assert(!result.input_passthrough);
}


static void test_dc_removal_subtracts_the_block_mean(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL
    };
    float samples[] = { 5.0f, 7.0f, 9.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_DC_REMOVAL);
    assert(nearly_equal(result.dc_offset_removed, 7.0f));
    assert(nearly_equal(samples[0], -2.0f));
    assert(nearly_equal(samples[1], 0.0f));
    assert(nearly_equal(samples[2], 2.0f));
}


static void test_energy_rms_measures_the_dc_removed_signal(void)
{
    /* Requesting both stages together proves execution order: RMS must see
     * the signal after DC removal, not the raw offset signal. Raw samples
     * have mean 4 and would give a much larger (and wrong) RMS if measured
     * before DC removal. */
    const seismic_dsp_config_t config = {
        .requested_stages =
            SEISMIC_DSP_STAGE_DC_REMOVAL | SEISMIC_DSP_STAGE_ENERGY_RMS
    };
    float samples[] = { 3.0f, 4.0f, 5.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    /* mean=4 -> DC-removed = [-1, 0, 1] -> RMS = sqrt((1+0+1)/3) */
    assert(nearly_equal(result.energy_rms, sqrtf(2.0f / 3.0f)));
}


static void test_energy_rms_of_pure_sine_block(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_ENERGY_RMS
    };
    static const size_t count = 1000u;
    float samples[1000];
    seismic_dsp_result_t result;

    for (size_t i = 0u; i < count; ++i) {
        samples[i] = sinf(2.0f * 3.14159265f * 10.0f * (float)i / 1000.0f);
    }

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, count, true, &result) == ESP_OK);
    /* RMS of a full-cycle sine of amplitude 1 is 1/sqrt(2). */
    assert(fabsf(result.energy_rms - 0.70710678f) < 0.01f);
}


static void test_normalization_scales_to_unit_peak(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_NORMALIZATION
    };
    float samples[] = { -2.0f, 1.0f, 4.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    assert(nearly_equal(result.peak_abs, 4.0f));
    assert(nearly_equal(samples[0], -0.5f));
    assert(nearly_equal(samples[1], 0.25f));
    assert(nearly_equal(samples[2], 1.0f));
}


static void test_normalization_of_all_zero_block_does_not_divide_by_zero(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_NORMALIZATION
    };
    float samples[] = { 0.0f, 0.0f, 0.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_NORMALIZATION);
    assert(result.peak_abs == 0.0f);
    assert(samples[0] == 0.0f && samples[1] == 0.0f && samples[2] == 0.0f);
    assert(!isnan(samples[0]) && !isinf(samples[0]));
}


static void test_windowing_tapers_block_edges_to_zero(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_WINDOWING
    };
    float samples[] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 5u, true, &result) == ESP_OK);
    assert(nearly_equal(samples[0], 0.0f));
    assert(nearly_equal(samples[4], 0.0f));
    assert(nearly_equal(samples[2], 1.0f));
    assert(samples[1] > 0.0f && samples[1] < 1.0f);
    assert(samples[3] > 0.0f && samples[3] < 1.0f);
}


static void test_windowing_single_sample_block_is_identity(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages = SEISMIC_DSP_STAGE_WINDOWING
    };
    float samples[] = { 3.5f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 1u, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_WINDOWING);
    assert(samples[0] == 3.5f);
}


static void test_full_stage_order_dc_rms_normalize_window(void)
{
    const seismic_dsp_config_t config = {
        .requested_stages =
            SEISMIC_DSP_STAGE_DC_REMOVAL |
            SEISMIC_DSP_STAGE_ENERGY_RMS |
            SEISMIC_DSP_STAGE_NORMALIZATION |
            SEISMIC_DSP_STAGE_WINDOWING
    };
    /* mean=1 -> DC-removed=[-2,0,2] -> RMS=sqrt(8/3) -> peak=2 ->
     * normalized=[-1,0,1] -> Hann(N=3)=[0,1,0] -> windowed=[0,0,0]. */
    float samples[] = { -1.0f, 1.0f, 3.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, true, &result) == ESP_OK);
    assert(nearly_equal(result.dc_offset_removed, 1.0f));
    assert(nearly_equal(result.energy_rms, sqrtf(8.0f / 3.0f)));
    assert(nearly_equal(result.peak_abs, 2.0f));
    assert(nearly_equal(samples[0], 0.0f));
    assert(nearly_equal(samples[1], 0.0f));
    assert(nearly_equal(samples[2], 0.0f));
    assert(!result.input_passthrough);
}


/*
 * bandpass_steady_state_gain() drives a 50-150 Hz (@ 1000 Hz) bandpass with a
 * pure tone across several contiguous blocks and reports the steady-state
 * (last-block) RMS gain relative to the tone's own 1/sqrt(2) RMS. Values
 * below were cross-checked against an independent Python simulation of the
 * exact same biquad math before being pinned as test tolerances:
 * center (86.603 Hz) -> ~1.0013, 2 Hz -> ~0.026, 450 Hz -> ~0.051.
 */
static float bandpass_steady_state_gain(float tone_hz)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f }
    };
    static const size_t block_size = 500u;
    static const size_t block_count = 5u;
    float block[500];
    seismic_dsp_result_t result;
    size_t sample_index = 0u;

    assert(seismic_dsp_init(&config) == ESP_OK);

    for (size_t b = 0u; b < block_count; ++b) {
        for (size_t i = 0u; i < block_size; ++i) {
            const float t = (float)sample_index / 1000.0f;
            block[i] = sinf(TEST_TWO_PI * tone_hz * t);
            ++sample_index;
        }
        assert(seismic_dsp_process_block(block, block_size, b != 0u, &result) == ESP_OK);
        assert(result.completed_stages == SEISMIC_DSP_STAGE_BANDPASS);
        assert(nearly_equal(result.bandpass_low_cutoff_hz, 50.0f));
        assert(nearly_equal(result.bandpass_high_cutoff_hz, 150.0f));
    }

    for (size_t i = 0u; i < block_size; ++i) {
        assert(!isnan(block[i]) && !isinf(block[i]));
    }

    return rms_of(block, block_size) * 1.41421356f;
}


static void test_bandpass_passes_its_own_center_frequency(void)
{
    const float gain = bandpass_steady_state_gain(86.60254f); /* sqrt(50*150) */
    assert(gain > 0.85f && gain < 1.15f);
}


static void test_bandpass_attenuates_far_below_the_band(void)
{
    const float gain = bandpass_steady_state_gain(2.0f);
    assert(gain < 0.15f);
}


static void test_bandpass_attenuates_far_above_the_band(void)
{
    const float gain = bandpass_steady_state_gain(450.0f);
    assert(gain < 0.15f);
}


/*
 * notch_steady_state_gain() mirrors bandpass_steady_state_gain() for a
 * 100 Hz / Q=10 (@ 1000 Hz) notch. Cross-checked the same way:
 * center (100 Hz) -> ~0.0000, 50 Hz away -> ~0.998.
 */
static float notch_steady_state_gain(float tone_hz)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 100.0f, .q_factor = 10.0f }
    };
    static const size_t block_size = 500u;
    static const size_t block_count = 5u;
    float block[500];
    seismic_dsp_result_t result;
    size_t sample_index = 0u;

    assert(seismic_dsp_init(&config) == ESP_OK);

    for (size_t b = 0u; b < block_count; ++b) {
        for (size_t i = 0u; i < block_size; ++i) {
            const float t = (float)sample_index / 1000.0f;
            block[i] = sinf(TEST_TWO_PI * tone_hz * t);
            ++sample_index;
        }
        assert(seismic_dsp_process_block(block, block_size, b != 0u, &result) == ESP_OK);
        assert(result.completed_stages == SEISMIC_DSP_STAGE_NOTCH);
        assert(nearly_equal(result.notch_center_hz, 100.0f));
        assert(nearly_equal(result.notch_q_factor, 10.0f));
    }

    for (size_t i = 0u; i < block_size; ++i) {
        assert(!isnan(block[i]) && !isinf(block[i]));
    }

    return rms_of(block, block_size) * 1.41421356f;
}


static void test_notch_rejects_its_own_center_frequency(void)
{
    const float gain = notch_steady_state_gain(100.0f);
    assert(gain < 0.05f);
}


static void test_notch_passes_frequencies_away_from_center(void)
{
    const float gain = notch_steady_state_gain(50.0f);
    assert(gain > 0.9f);
}


static void test_bandpass_impulse_response_is_finite_and_decays(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f }
    };
    static const size_t block_size = 200u;
    static const size_t block_count = 5u;
    float block[200];
    seismic_dsp_result_t result;
    float last_block_peak = 0.0f;

    assert(seismic_dsp_init(&config) == ESP_OK);

    for (size_t b = 0u; b < block_count; ++b) {
        for (size_t i = 0u; i < block_size; ++i) {
            block[i] = (b == 0u && i == 0u) ? 1.0f : 0.0f;
        }
        assert(seismic_dsp_process_block(block, block_size, b != 0u, &result) == ESP_OK);
        for (size_t i = 0u; i < block_size; ++i) {
            assert(!isnan(block[i]) && !isinf(block[i]));
        }
        if (b == block_count - 1u) {
            for (size_t i = 0u; i < block_size; ++i) {
                const float magnitude = fabsf(block[i]);
                if (magnitude > last_block_peak) {
                    last_block_peak = magnitude;
                }
            }
        }
    }

    /* A stable biquad's impulse response must decay; by the 5th 200-sample
     * block (1000 samples in) it is expected to be indistinguishable from
     * zero at float32 precision. */
    assert(last_block_peak < 1e-6f);
}


static void test_cadence_contiguous_false_resets_filter_state(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_BANDPASS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f }
    };
    seismic_dsp_result_t result;
    float warmup[8];
    for (size_t i = 0u; i < 8u; ++i) {
        warmup[i] = (i == 0u) ? 1.0f : 0.0f;
    }

    float probe_reset_after_warmup[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float probe_continued_after_warmup[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float probe_fresh[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    /* Flow 1: warm up filter state with an impulse, then reset via
     * cadence_contiguous=false before probing. */
    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(warmup, 8u, false, &result) == ESP_OK);
    assert(seismic_dsp_process_block(probe_reset_after_warmup, 4u, false, &result) == ESP_OK);
    assert(result.cadence_reset_applied);

    /* Flow 2: same warmup, but continue (no reset) into the probe -- carried
     * state must change the result relative to Flow 3. */
    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(warmup, 8u, false, &result) == ESP_OK);
    assert(seismic_dsp_process_block(probe_continued_after_warmup, 4u, true, &result) == ESP_OK);
    assert(!result.cadence_reset_applied);

    /* Flow 3: fresh filter, no warmup at all, probed directly. */
    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(probe_fresh, 4u, false, &result) == ESP_OK);

    for (size_t i = 0u; i < 4u; ++i) {
        assert(nearly_equal(probe_reset_after_warmup[i], probe_fresh[i]));
    }

    bool continued_probe_differs = false;
    for (size_t i = 0u; i < 4u; ++i) {
        if (!nearly_equal(probe_continued_after_warmup[i], probe_fresh[i])) {
            continued_probe_differs = true;
        }
    }
    assert(continued_probe_differs);
}


static void test_cadence_contiguous_false_resets_notch_state(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_NOTCH,
        .notch = { .center_hz = 100.0f, .q_factor = 10.0f }
    };
    seismic_dsp_result_t result;
    float warmup[8];
    for (size_t i = 0u; i < 8u; ++i) {
        warmup[i] = (i == 0u) ? 1.0f : 0.0f;
    }

    float reset_probe[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float continued_probe[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float fresh_probe[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(warmup, 8u, false, &result) == ESP_OK);
    assert(seismic_dsp_process_block(reset_probe, 4u, false, &result) == ESP_OK);
    assert(result.cadence_reset_applied);

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(warmup, 8u, false, &result) == ESP_OK);
    assert(seismic_dsp_process_block(continued_probe, 4u, true, &result) == ESP_OK);
    assert(!result.cadence_reset_applied);

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(fresh_probe, 4u, false, &result) == ESP_OK);

    for (size_t i = 0u; i < 4u; ++i) {
        assert(nearly_equal(reset_probe[i], fresh_probe[i]));
    }

    bool continued_probe_differs = false;
    for (size_t i = 0u; i < 4u; ++i) {
        if (!nearly_equal(continued_probe[i], fresh_probe[i])) {
            continued_probe_differs = true;
        }
    }
    assert(continued_probe_differs);
}


static void test_fft_reports_dominant_bin_aligned_frequency(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_FFT
    };
    static const size_t count = 1000u;
    float samples[1000];
    seismic_dsp_result_t result;

    /* 50 Hz at Fs=1000Hz/N=1000 lands exactly on bin 50: no interpolation
     * error to budget for. */
    for (size_t i = 0u; i < count; ++i) {
        samples[i] = sinf(TEST_TWO_PI * 50.0f * (float)i / 1000.0f);
    }

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, count, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_FFT);
    assert(fabsf(result.fft_dominant_frequency_hz - 50.0f) < 1.0f);
    assert(result.fft_dominant_magnitude > 0.0f);

    /* FFT is diagnostic-only: it must not mutate the block. */
    for (size_t i = 0u; i < count; ++i) {
        const float expected = sinf(TEST_TWO_PI * 50.0f * (float)i / 1000.0f);
        assert(nearly_equal(samples[i], expected));
    }
}


static void test_fft_dominant_frequency_is_unscaled_when_sample_rate_unknown(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 0u,
        .requested_stages = SEISMIC_DSP_STAGE_FFT
    };
    static const size_t count = 1000u;
    float samples[1000];
    seismic_dsp_result_t result;

    for (size_t i = 0u; i < count; ++i) {
        samples[i] = sinf(TEST_TWO_PI * 50.0f * (float)i / 1000.0f);
    }

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, count, true, &result) == ESP_OK);
    assert(result.fft_dominant_frequency_hz == 0.0f);
    assert(result.fft_dominant_magnitude > 0.0f);
}


static void test_fft_short_block_reports_zero_rather_than_garbage(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_FFT
    };
    float samples[] = { 3.5f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 1u, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_FFT);
    assert(result.fft_dominant_frequency_hz == 0.0f);
    assert(result.fft_dominant_magnitude == 0.0f);
}


static void test_fft_zero_block_has_no_fake_dominant_bin(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 1000u,
        .requested_stages = SEISMIC_DSP_STAGE_FFT
    };
    float samples[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 4u, true, &result) == ESP_OK);
    assert(result.completed_stages == SEISMIC_DSP_STAGE_FFT);
    assert(result.fft_dominant_frequency_hz == 0.0f);
    assert(result.fft_dominant_magnitude == 0.0f);
}


static void test_fft_runs_after_normalization(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 4u,
        .requested_stages =
            SEISMIC_DSP_STAGE_NORMALIZATION | SEISMIC_DSP_STAGE_FFT
    };
    float samples[] = { 0.0f, 2.0f, 0.0f, -2.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 4u, true, &result) == ESP_OK);
    assert(result.completed_stages == config.requested_stages);
    assert(nearly_equal(result.peak_abs, 2.0f));
    assert(nearly_equal(result.fft_dominant_frequency_hz, 1.0f));
    assert(nearly_equal(result.fft_dominant_magnitude, 2.0f));
    assert(nearly_equal(samples[1], 1.0f));
    assert(nearly_equal(samples[3], -1.0f));
}


/*
 * A synthetic proxy for a recorded AD7606 + ZF-24HP capture: a 50 Hz signal
 * of interest (inside the 50-150 Hz passband used elsewhere in this file)
 * riding on strong out-of-band content (a slow ~5 Hz drift and a ~400 Hz
 * component), typical of what an unfiltered geophone channel looks like.
 * This is deliberately NOT real sensor data -- no ZF-24HP characterization
 * exists in this repo yet (see MAX-133/MAX-132) -- but it exercises the same
 * "load a fixed regression vector through the pipeline" shape a real
 * recorded fixture will use once one is captured, and pins a real, checked
 * DSP behavior (out-of-band rejection) rather than just "does not crash".
 */
static void build_synthetic_recorded_style_fixture(float *out, size_t count, uint32_t sample_rate_hz)
{
    for (size_t i = 0u; i < count; ++i) {
        const float t = (float)i / (float)sample_rate_hz;
        const float signal_of_interest = 1.0f * sinf(TEST_TWO_PI * 50.0f * t);
        const float slow_drift = 4.0f * sinf(TEST_TWO_PI * 5.0f * t);
        const float high_frequency_noise = 2.0f * sinf(TEST_TWO_PI * 400.0f * t);
        out[i] = signal_of_interest + slow_drift + high_frequency_noise;
    }
}


static void test_synthetic_recorded_style_fixture_regression(void)
{
    static const uint32_t sample_rate_hz = 1000u;
    static const size_t block_size = 500u;
    static const size_t block_count = 4u;
    static const size_t total = 2000u; /* block_size * block_count */

    const seismic_dsp_config_t raw_config = {
        .sample_rate_hz = sample_rate_hz,
        .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL | SEISMIC_DSP_STAGE_ENERGY_RMS
    };
    const seismic_dsp_config_t filtered_config = {
        .sample_rate_hz = sample_rate_hz,
        .requested_stages =
            SEISMIC_DSP_STAGE_DC_REMOVAL |
            SEISMIC_DSP_STAGE_BANDPASS |
            SEISMIC_DSP_STAGE_NOTCH |
            SEISMIC_DSP_STAGE_ENERGY_RMS,
        .bandpass = { .low_cutoff_hz = 50.0f, .high_cutoff_hz = 150.0f },
        .notch = { .center_hz = 400.0f, .q_factor = 10.0f }
    };

    float fixture[2000];
    float raw_block[500];
    float filtered_block[500];
    seismic_dsp_result_t result;
    float raw_energy = 0.0f;
    float filtered_energy = 0.0f;

    build_synthetic_recorded_style_fixture(fixture, total, sample_rate_hz);

    assert(seismic_dsp_init(&raw_config) == ESP_OK);
    for (size_t b = 0u; b < block_count; ++b) {
        for (size_t i = 0u; i < block_size; ++i) {
            raw_block[i] = fixture[b * block_size + i];
        }
        assert(seismic_dsp_process_block(raw_block, block_size, b != 0u, &result) == ESP_OK);
        if (b == block_count - 1u) {
            raw_energy = result.energy_rms;
        }
    }

    assert(seismic_dsp_init(&filtered_config) == ESP_OK);
    for (size_t b = 0u; b < block_count; ++b) {
        for (size_t i = 0u; i < block_size; ++i) {
            filtered_block[i] = fixture[b * block_size + i];
        }
        assert(seismic_dsp_process_block(filtered_block, block_size, b != 0u, &result) == ESP_OK);
        for (size_t i = 0u; i < block_size; ++i) {
            assert(!isnan(filtered_block[i]) && !isinf(filtered_block[i]));
        }
        if (b == block_count - 1u) {
            filtered_energy = result.energy_rms;
        }
    }

    assert(raw_energy > 0.0f);
    assert(filtered_energy > 0.0f);
    /* The fixture's out-of-band content (5 Hz drift, 400 Hz noise) outweighs
     * its 50 Hz signal of interest; band-limiting must measurably shrink
     * total energy. */
    assert(filtered_energy < 0.7f * raw_energy);
}


int main(void)
{
    test_requires_initialization();
    test_rejects_invalid_arguments();
    test_rejects_unknown_stage_bits();
    test_bandpass_requires_valid_corners();
    test_notch_requires_valid_parameters();
    test_empty_block_leaves_requested_stages_pending();
    test_stateless_gap_does_not_claim_filter_reset();
    test_unrequested_stages_stay_placeholder_and_passthrough();
    test_unimplemented_stages_remain_placeholders();
    test_dc_removal_subtracts_the_block_mean();
    test_energy_rms_measures_the_dc_removed_signal();
    test_energy_rms_of_pure_sine_block();
    test_normalization_scales_to_unit_peak();
    test_normalization_of_all_zero_block_does_not_divide_by_zero();
    test_windowing_tapers_block_edges_to_zero();
    test_windowing_single_sample_block_is_identity();
    test_full_stage_order_dc_rms_normalize_window();
    test_bandpass_passes_its_own_center_frequency();
    test_bandpass_attenuates_far_below_the_band();
    test_bandpass_attenuates_far_above_the_band();
    test_notch_rejects_its_own_center_frequency();
    test_notch_passes_frequencies_away_from_center();
    test_bandpass_impulse_response_is_finite_and_decays();
    test_cadence_contiguous_false_resets_filter_state();
    test_cadence_contiguous_false_resets_notch_state();
    test_fft_reports_dominant_bin_aligned_frequency();
    test_fft_dominant_frequency_is_unscaled_when_sample_rate_unknown();
    test_fft_short_block_reports_zero_rather_than_garbage();
    test_fft_zero_block_has_no_fake_dominant_bin();
    test_fft_runs_after_normalization();
    test_synthetic_recorded_style_fixture_regression();
    puts("seismic_dsp host tests passed");
    return 0;
}
