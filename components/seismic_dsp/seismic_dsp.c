#include <stdbool.h>

#include "seismic_dsp.h"


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


esp_err_t seismic_dsp_process_block(
    const float *input,
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

    *result = (seismic_dsp_result_t) {
        .sample_count = sample_count,
        .sample_rate_hz = s_config.sample_rate_hz,
        .requested_stages = s_config.requested_stages,
        .placeholder_stages = s_config.requested_stages,
        .completed_stages = 0u,
        .input_passthrough = true
    };

    return ESP_OK;
}
