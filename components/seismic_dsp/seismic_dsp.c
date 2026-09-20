#include <math.h>
#include <stdbool.h>

#include "seismic_dsp.h"


#define SEISMIC_DSP_TWO_PI 6.28318530717958647692f


static seismic_dsp_config_t s_config;
static bool s_initialized;


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

    s_config = *selected_config;
    s_initialized = true;
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


esp_err_t seismic_dsp_process_block(
    float *input,
    size_t sample_count,
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

    if (sample_count > 0u) {
        if ((requested & SEISMIC_DSP_STAGE_DC_REMOVAL) != 0u) {
            dc_offset_removed = apply_dc_removal(input, sample_count);
            completed |= SEISMIC_DSP_STAGE_DC_REMOVAL;
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
    }

    *result = (seismic_dsp_result_t) {
        .sample_count = sample_count,
        .sample_rate_hz = s_config.sample_rate_hz,
        .requested_stages = requested,
        .placeholder_stages = requested & ~completed,
        .completed_stages = completed,
        .input_passthrough = (completed == 0u),
        .dc_offset_removed = dc_offset_removed,
        .energy_rms = energy_rms,
        .peak_abs = peak_abs
    };

    return ESP_OK;
}
