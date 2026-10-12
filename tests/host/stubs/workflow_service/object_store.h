#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OBJECT_STORE_ID_SIZE 16U

typedef struct {
    bool valid;
    double latitude;
    double longitude;
    float accuracy_m;
    bool altitude_valid;
    double altitude_m;
    float altitude_accuracy_m;
    uint64_t captured_at_ms;
    char source[24];
} object_store_location_t;

esp_err_t object_store_scan_present(const char *object_id, const char *session_id, bool *present_out);
esp_err_t object_store_set_location(const char *object_id, const object_store_location_t *location);
esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out);
esp_err_t object_store_get_calc_settings_json_cached(const char *object_id, char **json_out);
esp_err_t object_store_set_calc_settings_json(const char *object_id, const char *json);
