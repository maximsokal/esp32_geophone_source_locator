#pragma once

#include <stdbool.h>

#include "storage_service.h"


typedef enum
{
    STORAGE_COMMAND_INFO = 0,
    STORAGE_COMMAND_ERASE_DATA,
    STORAGE_COMMAND_STOP

} storage_command_t;


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
