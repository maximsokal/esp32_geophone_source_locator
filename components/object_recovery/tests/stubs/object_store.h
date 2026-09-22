#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_format.h"

#define OBJECT_STORE_POD_COUNT 4

typedef struct
{
    bool computed;
    bool leveled;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    uint64_t computed_at_ms;

} object_store_pod_geometry_t;

typedef struct
{
    bool anchored;
    object_store_pod_geometry_t pods[OBJECT_STORE_POD_COUNT];

} object_store_geometry_t;

typedef struct
{
    object_summary_t summary;

} object_store_profile_t;

static bool object_recovery_test_object_leased = false;

esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
);

static inline esp_err_t object_store_object_is_leased(
    const char *object_id,
    bool *leased
)
{
    (void)object_id;
    if (leased == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *leased = object_recovery_test_object_leased;
    return ESP_OK;
}
