#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "object_format.h"

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
