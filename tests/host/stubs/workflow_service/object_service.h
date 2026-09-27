#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define OBJECT_SERVICE_OBJECT_ID_SIZE 40U

typedef struct {
    bool active;
    char id[OBJECT_SERVICE_OBJECT_ID_SIZE];
} object_service_summary_t;

esp_err_t object_service_get_active(object_service_summary_t *object);
