#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct {
    uint32_t log_type;
    const char *auth_key;
} esp_insights_config_t;
esp_err_t esp_insights_init(const esp_insights_config_t *config);
