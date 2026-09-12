#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "seismic_dsp.h"


static void test_requires_initialization(void)
{
    seismic_dsp_result_t result;
    const float sample = 1.0f;

    assert(seismic_dsp_process_block(&sample, 1u, &result) == ESP_ERR_INVALID_STATE);
}


static void test_rejects_invalid_arguments(void)
{
    const float sample = 1.0f;
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(NULL) == ESP_OK);
    assert(seismic_dsp_process_block(&sample, 1u, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 1u, &result) == ESP_ERR_INVALID_ARG);
    assert(seismic_dsp_process_block(NULL, 0u, &result) == ESP_OK);
}


static void test_reports_requested_stages_as_placeholders(void)
{
    const seismic_dsp_config_t config = {
        .sample_rate_hz = 500u,
        .requested_stages = SEISMIC_DSP_STAGE_ALL
    };
    const float samples[] = { -1.0f, 0.0f, 1.0f };
    seismic_dsp_result_t result;

    assert(seismic_dsp_init(&config) == ESP_OK);
    assert(seismic_dsp_process_block(samples, 3u, &result) == ESP_OK);
    assert(result.sample_count == 3u);
    assert(result.sample_rate_hz == 500u);
    assert(result.requested_stages == SEISMIC_DSP_STAGE_ALL);
    assert(result.placeholder_stages == SEISMIC_DSP_STAGE_ALL);
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


int main(void)
{
    test_requires_initialization();
    test_rejects_invalid_arguments();
    test_reports_requested_stages_as_placeholders();
    test_rejects_unknown_stage_bits();
    puts("seismic_dsp host tests passed");
    return 0;
}
