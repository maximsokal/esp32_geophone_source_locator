#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "cJSON.h"
#include "localization_calculate.h"

/* Standard processing defaults use the selected acquisition source's rate. */
esp_err_t localization_config_standard_processing(
    localization_calculate_config_t *config_out,
    uint32_t *budget_ms_out
);

/* Parse the explicit calculation fields accepted by calculate_source. */
esp_err_t localization_config_from_json(
    const cJSON *root,
    localization_calculate_config_t *config_out,
    uint32_t *budget_ms_out
);

/* Caller releases json_out with cJSON_free(). */
esp_err_t localization_config_to_json(
    const localization_calculate_config_t *config,
    const char *material_id,
    uint32_t budget_ms,
    char **json_out
);

/* Processing fields only; used by GET /api/localization/runs?job=settings. */
esp_err_t localization_config_standard_processing_to_json(char **json_out);
