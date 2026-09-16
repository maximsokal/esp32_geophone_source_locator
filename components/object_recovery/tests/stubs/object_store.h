#pragma once

#include <stdbool.h>

#include "esp_err.h"

static bool object_recovery_test_object_leased = false;

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
