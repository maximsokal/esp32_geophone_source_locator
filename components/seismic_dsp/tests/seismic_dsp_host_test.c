#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "seismic_dsp.h"


#define EPSILON 0.0001f

static bool nearly_equal(float a, float b)
{
    return fabsf(a - b) < EPSILON;
}


_Static_assert(
    SEISMIC_DSP_FIXED_RATE_REQUIRES_CADENCE_VALID_INPUT == 1u,
    "fixed-rate DSP must require cadence-valid input"
);


static void test_requires_initialization(void)
{
    seismic_dsp_result_t result;
    float sample = 1.0f;

    assert(seismic_dsp_process_block(&sample, 1u, &result) == ESP_ERR_INVALID_STATE);
}


static void test_rejects_invalid_arguments(void)
{
    float sample = 1.0f;
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(NULL) == ESP_OK);
    assert(seismic_dsp_process_block(&sample, 1u, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 1u, &result) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 0u, &result) == ESP_OK);
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


static void test_unrequested_stages_stay_placeholder_and_passthrough(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 500u,
        .requested_stages = 0u
    };
    float samples[] = { -1.0f, 0.0f, 1.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
        .requested_stages = SEISMIC_DSP_STAGE_ALL
    };
    float samples[] = { -1.0f, 0.0f, 1.0f };
    seismic_dsp_result_t result;

    const uint32_t implemented =
        SEISMIC_DSP_STAGE_DC_REMOVAL |
        SEISMIC_DSP_STAGE_ENERGY_RMS |
        SEISMIC_DSP_STAGE_NORMALIZATION |
        SEISMIC_DSP_STAGE_WINDOWING;
    const uint32_t still_pending =
        SEISMIC_DSP_STAGE_FIR |
        SEISMIC_DSP_STAGE_IIR |
        SEISMIC_DSP_STAGE_BANDPASS |
        SEISMIC_DSP_STAGE_NOTCH |
        SEISMIC_DSP_STAGE_FFT |
        SEISMIC_DSP_STAGE_CROSS_CORRELATION;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, count, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 5u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 1u, &result) == ESP_OK);
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
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
    assert(nearly_equal(result.dc_offset_removed, 1.0f));
    assert(nearly_equal(result.energy_rms, sqrtf(8.0f / 3.0f)));
    assert(nearly_equal(result.peak_abs, 2.0f));
    assert(nearly_equal(samples[0], 0.0f));
    assert(nearly_equal(samples[1], 0.0f));
    assert(nearly_equal(samples[2], 0.0f));
    assert(!result.input_passthrough);
}


int main(void)
{
    test_requires_initialization();
    test_rejects_invalid_arguments();
    test_rejects_unknown_stage_bits();
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
    puts("seismic_dsp host tests passed");
    return 0;
}
