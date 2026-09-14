#pragma once

#include <stdint.h>

#include "esp_err.h"


static inline esp_err_t seismic_recorder_storage_preflight(
    uint32_t sample_rate_hz
)
{
    return sample_rate_hz == 0U
        ? ESP_ERR_INVALID_ARG
        : ESP_OK;
}
