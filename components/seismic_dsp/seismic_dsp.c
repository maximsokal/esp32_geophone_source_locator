#include <math.h>
#include <stdbool.h>

#include "seismic_dsp.h"


#define SEISMIC_DSP_TWO_PI 6.28318530717958647692f
#define SEISMIC_DSP_TWO_PI_D 6.283185307179586476925286766559


static seismic_dsp_config_t s_config;
static bool s_initialized;


typedef struct
{
    float b0, b1, b2, a1, a2;
} seismic_dsp_biquad_coeffs_t;

typedef struct
{
    float w1, w2;
} seismic_dsp_biquad_state_t;

static seismic_dsp_biquad_coeffs_t s_bandpass_coeffs;
static seismic_dsp_biquad_state_t s_bandpass_state;
static seismic_dsp_biquad_coeffs_t s_notch_coeffs;
static seismic_dsp_biquad_state_t s_notch_state;


/*
 * Constant 0 dB peak-gain bandpass biquad (RBJ Audio EQ Cookbook form),
 * bilinear-transformed from the caller's own corner frequencies. center_hz
 * is the geometric mean of the corners and q is center_hz / bandwidth_hz --
 * both deterministic functions of the caller's input, not invented
 * constants. Computed in double precision since this runs once per
 * seismic_dsp_init() call, not per sample.
 */
static void design_bandpass(
    float low_cutoff_hz,
    float high_cutoff_hz,
    uint32_t sample_rate_hz,
    seismic_dsp_biquad_coeffs_t *out
)
{
    const double center_hz =
        sqrt((double)low_cutoff_hz * (double)high_cutoff_hz);
    const double bandwidth_hz = (double)high_cutoff_hz - (double)low_cutoff_hz;
    const double q = center_hz / bandwidth_hz;
    const double w0 = SEISMIC_DSP_TWO_PI_D * center_hz / (double)sample_rate_hz;
    const double cos_w0 = cos(w0);
    const double sin_w0 = sin(w0);
    const double alpha = sin_w0 / (2.0 * q);
    const double a0 = 1.0 + alpha;

    out->b0 = (float)(alpha / a0);
    out->b1 = 0.0f;
    out->b2 = (float)(-alpha / a0);
    out->a1 = (float)((-2.0 * cos_w0) / a0);
    out->a2 = (float)((1.0 - alpha) / a0);
}


/*
 * Band-reject (notch) biquad (RBJ Audio EQ Cookbook form), bilinear-
 * transformed from the caller's own center frequency and Q. Computed in
 * double precision for the same reason as design_bandpass().
 */
static void design_notch(
    float center_hz,
    float q_factor,
    uint32_t sample_rate_hz,
    seismic_dsp_biquad_coeffs_t *out
)
{
    const double w0 =
        SEISMIC_DSP_TWO_PI_D * (double)center_hz / (double)sample_rate_hz;
    const double cos_w0 = cos(w0);
    const double sin_w0 = sin(w0);
    const double alpha = sin_w0 / (2.0 * (double)q_factor);
    const double a0 = 1.0 + alpha;

    out->b0 = (float)(1.0 / a0);
    out->b1 = (float)((-2.0 * cos_w0) / a0);
    out->b2 = (float)(1.0 / a0);
    out->a1 = (float)((-2.0 * cos_w0) / a0);
    out->a2 = (float)((1.0 - alpha) / a0);
}


/*
 * Direct Form II Transposed biquad, applied in place. This form carries
 * exactly two state variables regardless of coefficients, and is the
 * numerically well-behaved standard for a single persistent biquad section.
 */
static void apply_biquad(
    float *input,
    size_t sample_count,
    const seismic_dsp_biquad_coeffs_t *coeffs,
    seismic_dsp_biquad_state_t *state
)
{
    for (size_t i = 0u; i < sample_count; ++i) {
        const float x = input[i];
        const float y = coeffs->b0 * x + state->w1;
        state->w1 = coeffs->b1 * x - coeffs->a1 * y + state->w2;
        state->w2 = coeffs->b2 * x - coeffs->a2 * y;
        input[i] = y;
    }
}


static bool bandpass_config_is_valid(
    const seismic_dsp_bandpass_config_t *config,
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0u) {
        return false;
    }
    if (!(config->low_cutoff_hz > 0.0f)) {
        return false;
    }
    if (!(config->high_cutoff_hz > config->low_cutoff_hz)) {
        return false;
    }
    return config->high_cutoff_hz < (float)sample_rate_hz / 2.0f;
}


static bool notch_config_is_valid(
    const seismic_dsp_notch_config_t *config,
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0u) {
        return false;
    }
    if (!(config->center_hz > 0.0f)) {
        return false;
    }
    if (!(config->q_factor > 0.0f)) {
        return false;
    }
    return config->center_hz < (float)sample_rate_hz / 2.0f;
}


esp_err_t seismic_dsp_init(
    const seismic_dsp_config_t *config
)
{
    const seismic_dsp_config_t empty_config = { 0 };
    const seismic_dsp_config_t *selected_config =
        config == NULL ? &empty_config : config;

    if ((selected_config->requested_stages & ~SEISMIC_DSP_STAGE_ALL) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((selected_config->requested_stages & SEISMIC_DSP_STAGE_BANDPASS) != 0u
        && !bandpass_config_is_valid(
               &selected_config->bandpass, selected_config->sample_rate_hz)) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((selected_config->requested_stages & SEISMIC_DSP_STAGE_NOTCH) != 0u
        && !notch_config_is_valid(
               &selected_config->notch, selected_config->sample_rate_hz)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *selected_config;
    s_initialized = true;

    if ((s_config.requested_stages & SEISMIC_DSP_STAGE_BANDPASS) != 0u) {
        design_bandpass(
            s_config.bandpass.low_cutoff_hz,
            s_config.bandpass.high_cutoff_hz,
            s_config.sample_rate_hz,
            &s_bandpass_coeffs);
    }
    if ((s_config.requested_stages & SEISMIC_DSP_STAGE_NOTCH) != 0u) {
        design_notch(
            s_config.notch.center_hz,
            s_config.notch.q_factor,
            s_config.sample_rate_hz,
            &s_notch_coeffs);
    }

    /* A (re)configuration always starts a new acquisition run: see the
     * CADENCE CONTRACT in seismic_dsp.h. */
    s_bandpass_state = (seismic_dsp_biquad_state_t) { 0 };
    s_notch_state = (seismic_dsp_biquad_state_t) { 0 };

    return ESP_OK;
}


/*
 * Per-block mean removal. Deliberately stateless across blocks: see the
 * CADENCE CONTRACT in seismic_dsp.h. A stage that keeps no cross-block state
 * cannot leak state across an acquisition gap it was never told about.
 */
static float apply_dc_removal(
    float *input,
    size_t sample_count
)
{
    double sum = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        sum += (double)input[i];
    }
    const float mean = (float)(sum / (double)sample_count);

    for (size_t i = 0u; i < sample_count; ++i) {
        input[i] -= mean;
    }

    return mean;
}


static float apply_energy_rms(
    const float *input,
    size_t sample_count
)
{
    double sum_squares = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        sum_squares += (double)input[i] * (double)input[i];
    }

    return (float)sqrt(sum_squares / (double)sample_count);
}


/*
 * Peak-magnitude normalization. An all-zero (or otherwise degenerate) block
 * has no peak to scale by; it is left unscaled rather than divided by zero,
 * and peak_abs reports 0 so callers can distinguish that case.
 */
static float apply_normalization(
    float *input,
    size_t sample_count
)
{
    float peak_abs = 0.0f;
    for (size_t i = 0u; i < sample_count; ++i) {
        const float magnitude = fabsf(input[i]);
        if (magnitude > peak_abs) {
            peak_abs = magnitude;
        }
    }

    if (peak_abs <= 0.0f) {
        return 0.0f;
    }

    const float scale = 1.0f / peak_abs;
    for (size_t i = 0u; i < sample_count; ++i) {
        input[i] *= scale;
    }

    return peak_abs;
}


/*
 * Periodic Hann window, applied in place. A single-sample block has no
 * meaningful window shape, so it is left unscaled (identity) rather than
 * dividing by a zero-length denominator.
 */
static void apply_windowing(
    float *input,
    size_t sample_count
)
{
    if (sample_count < 2u) {
        return;
    }

    const float denominator = (float)(sample_count - 1u);
    for (size_t i = 0u; i < sample_count; ++i) {
        const float phase =
            SEISMIC_DSP_TWO_PI * (float)i / denominator;
        const float coefficient = 0.5f * (1.0f - cosf(phase));
        input[i] *= coefficient;
    }
}


/*
 * Diagnostic-only direct DFT magnitude search over the non-DC bins
 * [1, sample_count/2]. O(sample_count^2); intended for occasional,
 * modest-sized diagnostic blocks rather than continuous high-rate use. The
 * DC bin (k=0) is deliberately excluded from the search: a residual DC/very
 * low-frequency component would trivially dominate and is not the
 * "dominant frequency" a diagnostic caller is looking for.
 */
static void find_dominant_frequency(
    const float *input,
    size_t sample_count,
    uint32_t sample_rate_hz,
    float *out_frequency_hz,
    float *out_magnitude
)
{
    *out_frequency_hz = 0.0f;
    *out_magnitude = 0.0f;

    if (sample_count < 2u) {
        return;
    }

    const size_t nyquist_bin = sample_count / 2u;
    double best_magnitude = 0.0;
    size_t best_bin = 0u;

    for (size_t k = 1u; k <= nyquist_bin; ++k) {
        double real = 0.0;
        double imag = 0.0;
        const double angular_step =
            -SEISMIC_DSP_TWO_PI_D * (double)k / (double)sample_count;
        for (size_t n = 0u; n < sample_count; ++n) {
            const double angle = angular_step * (double)n;
            real += (double)input[n] * cos(angle);
            imag += (double)input[n] * sin(angle);
        }
        const double magnitude = sqrt(real * real + imag * imag);
        if (magnitude > best_magnitude) {
            best_magnitude = magnitude;
            best_bin = k;
        }
    }

    *out_magnitude = (float)best_magnitude;
    if (sample_rate_hz > 0u) {
        *out_frequency_hz =
            (float)best_bin * (float)sample_rate_hz / (float)sample_count;
    }
}


esp_err_t seismic_dsp_process_block(
    float *input,
    size_t sample_count,
    bool cadence_contiguous,
    seismic_dsp_result_t *result
)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (input == NULL && sample_count != 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t requested = s_config.requested_stages;
    uint32_t completed = 0u;
    float dc_offset_removed = 0.0f;
    float energy_rms = 0.0f;
    float peak_abs = 0.0f;
    float bandpass_low = 0.0f;
    float bandpass_high = 0.0f;
    float notch_center = 0.0f;
    float notch_q = 0.0f;
    float fft_frequency = 0.0f;
    float fft_magnitude = 0.0f;

    const bool cadence_reset_applied = !cadence_contiguous;
    if (cadence_reset_applied) {
        s_bandpass_state = (seismic_dsp_biquad_state_t) { 0 };
        s_notch_state = (seismic_dsp_biquad_state_t) { 0 };
    }

    if (sample_count > 0u) {
        if ((requested & SEISMIC_DSP_STAGE_DC_REMOVAL) != 0u) {
            dc_offset_removed = apply_dc_removal(input, sample_count);
            completed |= SEISMIC_DSP_STAGE_DC_REMOVAL;
        }

        if ((requested & SEISMIC_DSP_STAGE_BANDPASS) != 0u) {
            apply_biquad(input, sample_count, &s_bandpass_coeffs, &s_bandpass_state);
            completed |= SEISMIC_DSP_STAGE_BANDPASS;
            bandpass_low = s_config.bandpass.low_cutoff_hz;
            bandpass_high = s_config.bandpass.high_cutoff_hz;
        }

        if ((requested & SEISMIC_DSP_STAGE_NOTCH) != 0u) {
            apply_biquad(input, sample_count, &s_notch_coeffs, &s_notch_state);
            completed |= SEISMIC_DSP_STAGE_NOTCH;
            notch_center = s_config.notch.center_hz;
            notch_q = s_config.notch.q_factor;
        }

        if ((requested & SEISMIC_DSP_STAGE_ENERGY_RMS) != 0u) {
            energy_rms = apply_energy_rms(input, sample_count);
            completed |= SEISMIC_DSP_STAGE_ENERGY_RMS;
        }

        if ((requested & SEISMIC_DSP_STAGE_NORMALIZATION) != 0u) {
            peak_abs = apply_normalization(input, sample_count);
            completed |= SEISMIC_DSP_STAGE_NORMALIZATION;
        }

        if ((requested & SEISMIC_DSP_STAGE_WINDOWING) != 0u) {
            apply_windowing(input, sample_count);
            completed |= SEISMIC_DSP_STAGE_WINDOWING;
        }

        if ((requested & SEISMIC_DSP_STAGE_FFT) != 0u) {
            find_dominant_frequency(
                input, sample_count, s_config.sample_rate_hz,
                &fft_frequency, &fft_magnitude);
            completed |= SEISMIC_DSP_STAGE_FFT;
        }
    }

    *result = (seismic_dsp_result_t) {
        .sample_count = sample_count,
        .sample_rate_hz = s_config.sample_rate_hz,
        .requested_stages = requested,
        .placeholder_stages = requested & ~completed,
        .completed_stages = completed,
        .input_passthrough = (completed == 0u),
        .cadence_reset_applied = cadence_reset_applied,
        .dc_offset_removed = dc_offset_removed,
        .energy_rms = energy_rms,
        .peak_abs = peak_abs,
        .bandpass_low_cutoff_hz = bandpass_low,
        .bandpass_high_cutoff_hz = bandpass_high,
        .notch_center_hz = notch_center,
        .notch_q_factor = notch_q,
        .fft_dominant_frequency_hz = fft_frequency,
        .fft_dominant_magnitude = fft_magnitude
    };

    return ESP_OK;
}
