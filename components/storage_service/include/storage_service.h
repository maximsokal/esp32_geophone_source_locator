#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "sd_card_info.h"


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


typedef void (*storage_service_event_callback_t)(
    const storage_service_event_t *event,
    void *user_context
);


esp_err_t storage_service_init(void);


void storage_service_shutdown(void);


esp_err_t storage_service_set_event_callback(
    storage_service_event_callback_t callback,
    void *user_context
);


esp_err_t storage_service_info_async(void);


esp_err_t storage_service_erase_data_async(void);


bool storage_service_is_busy(void);
