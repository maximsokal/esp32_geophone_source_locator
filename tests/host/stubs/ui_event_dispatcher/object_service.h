#pragma once

#include <stddef.h>

#include "esp_err.h"

#define OBJECT_SERVICE_MAX_OBJECTS 4U

typedef struct
{
    int unused;

} object_service_summary_t;

typedef struct
{
    int unused;

} object_service_event_t;

esp_err_t object_service_list_async(void);
