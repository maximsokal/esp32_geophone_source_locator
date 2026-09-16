#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { uint64_t capacity_bytes; } sd_card_info_t;

typedef enum
{
    STORAGE_SERVICE_EVENT_INFO_DONE = 0,
    STORAGE_SERVICE_EVENT_INFO_FAILED,
    STORAGE_SERVICE_EVENT_ERASE_DONE,
    STORAGE_SERVICE_EVENT_ERASE_FAILED
} storage_service_event_type_t;

typedef struct
{
    storage_service_event_type_t type;
    esp_err_t result;
    sd_card_info_t info;
} storage_service_event_t;

bool storage_service_process_info(storage_service_event_t *event);
bool storage_service_process_erase(storage_service_event_t *event);
