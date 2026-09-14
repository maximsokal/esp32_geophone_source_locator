#pragma once

#include "esp_err.h"

typedef struct {
    int unused;
} storage_service_event_t;

esp_err_t storage_service_info_async(void);
