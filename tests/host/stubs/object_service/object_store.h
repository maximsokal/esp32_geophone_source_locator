#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "object_format.h"

esp_err_t object_store_get_active(object_summary_t *object);
esp_err_t object_store_create(object_summary_t *object);
esp_err_t object_store_list(
    object_summary_t *objects,
    size_t capacity,
    size_t *count
);
