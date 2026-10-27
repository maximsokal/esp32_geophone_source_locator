#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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

#define OBJECT_STORE_POD_COUNT 4U
typedef struct { float cable_lengths_m[OBJECT_STORE_POD_COUNT]; } object_store_profile_t;
esp_err_t object_store_get_profile_cached(const char *object_id, object_store_profile_t *profile);

#define OBJECT_STORE_MAX_CS_PAIRS 10U
typedef struct {
    uint8_t node_a, node_b;
    bool valid;
    char state[24];
    int32_t distance_mm;
    uint16_t confidence, attempts;
    uint8_t valid_samples;
    int32_t spread_mm;
    uint8_t rejected_samples;
    uint16_t scheduler_quality_flags;
    int32_t automatic_distance_mm;
} object_store_cs_pair_t;
typedef struct { size_t pair_count; object_store_cs_pair_t pairs[OBJECT_STORE_MAX_CS_PAIRS]; } object_store_cs_pairs_t;
esp_err_t object_store_set_ranging_pairs(const char *object_id, const object_store_cs_pairs_t *pairs);
