#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"
#include "localization_calculate.h"

/* Parse the explicit calculation fields accepted by calculate_source. */
esp_err_t localization_config_from_json(
    const cJSON *root,
    localization_calculate_config_t *config_out,
    uint32_t *budget_ms_out
);
