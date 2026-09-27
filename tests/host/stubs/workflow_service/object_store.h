#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define OBJECT_STORE_ID_SIZE 16U

esp_err_t object_store_scan_present(const char *object_id, const char *session_id, bool *present_out);
esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out);
esp_err_t object_store_set_calc_settings_json(const char *object_id, const char *json);
