#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_format.h"

#define OBJECT_STORE_ID_SIZE OBJECT_FORMAT_ID_SIZE
#define OBJECT_STORE_NAME_SIZE OBJECT_FORMAT_NAME_SIZE
#define OBJECT_STORE_MAX_OBJECTS OBJECT_FORMAT_MAX_OBJECTS
#define OBJECT_STORE_POD_COUNT 4
#define OBJECT_STORE_LOCATION_SOURCE_SIZE 24
#define OBJECT_STORE_DEFAULT_CABLE_LENGTH_M 5.0f

typedef object_summary_t object_store_summary_t;

typedef struct
{
    bool valid;
    double latitude;
    double longitude;
    float accuracy_m;
    bool altitude_valid;
    double altitude_m;
    float altitude_accuracy_m;
    uint64_t captured_at_ms;
    char source[OBJECT_STORE_LOCATION_SOURCE_SIZE];
} object_store_location_t;

typedef struct
{
    object_store_summary_t summary;
    object_store_location_t location;
    float cable_lengths_m[OBJECT_STORE_POD_COUNT];
} object_store_profile_t;

esp_err_t object_store_init(void);
esp_err_t object_store_create(object_store_summary_t *created_object);
esp_err_t object_store_list(object_store_summary_t *objects, size_t max_objects, size_t *object_count);
esp_err_t object_store_list_profiles(object_store_profile_t *objects, size_t max_objects, size_t *object_count);
esp_err_t object_store_list_profiles_with_defaults(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
);

/*
 * Cooperative status/UI snapshot. Unlike the authoritative cold index build,
 * this releases the shared TFT/SD storage guard between object metadata files
 * so low-priority Web cache work cannot freeze the local LVGL UI for seconds.
 */
esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
);

esp_err_t object_store_get_active(object_store_summary_t *object);
esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile);
esp_err_t object_store_set_active(const char *object_id);
esp_err_t object_store_delete(const char *object_id);
esp_err_t object_store_rename(const char *object_id, const char *name);
esp_err_t object_store_set_location(const char *object_id, const object_store_location_t *location);
esp_err_t object_store_set_cable_length(const char *object_id, size_t pod_index, float length_m);
esp_err_t object_store_get_cable_defaults(float lengths_m[OBJECT_STORE_POD_COUNT]);
esp_err_t object_store_set_cable_default(size_t pod_index, float length_m);
esp_err_t object_store_reset(void);
