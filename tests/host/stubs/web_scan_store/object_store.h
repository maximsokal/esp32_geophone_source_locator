#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define OBJECT_STORE_ID_SIZE 16U

typedef struct {
    int unused;
} object_store_profile_t;

esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
);
