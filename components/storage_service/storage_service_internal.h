#pragma once

#include <stdbool.h>

#include "storage_service.h"

#include "freertos/FreeRTOS.h"


typedef enum
{
    STORAGE_COMMAND_INFO = 0,
    STORAGE_COMMAND_ERASE_DATA,
    STORAGE_COMMAND_STOP

} storage_command_t;


typedef struct
{
    portMUX_TYPE lock;
    storage_service_event_callback_t callback;
    void *context;
    unsigned in_flight;

} storage_service_callback_slot_t;


/* Component-private state exposure for lifecycle/host verification. */
storage_service_callback_slot_t *storage_service_internal_callback_slot(void);


/*
 * Return true when the worker should deliver the generated
 * event to the UI callback.
 */
bool storage_service_process_info(
    storage_service_event_t *event
);


bool storage_service_process_erase(
    storage_service_event_t *event
);
