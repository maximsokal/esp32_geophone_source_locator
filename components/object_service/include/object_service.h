#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "object_format.h"


#define OBJECT_SERVICE_OBJECT_ID_SIZE \
    OBJECT_FORMAT_ID_SIZE

#define OBJECT_SERVICE_MAX_OBJECTS \
    OBJECT_FORMAT_MAX_OBJECTS


typedef object_summary_t
    object_service_summary_t;


typedef enum
{
    OBJECT_SERVICE_EVENT_CREATE_DONE = 0,
    OBJECT_SERVICE_EVENT_CREATE_FAILED,

    OBJECT_SERVICE_EVENT_LIST_DONE,
    OBJECT_SERVICE_EVENT_LIST_FAILED,

    OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE,
    OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED,

    OBJECT_SERVICE_EVENT_DELETE_DONE,
    OBJECT_SERVICE_EVENT_DELETE_FAILED

} object_service_event_type_t;


typedef struct
{
    object_service_event_type_t type;

    esp_err_t result;

    object_service_summary_t object;

    char object_id[
        OBJECT_SERVICE_OBJECT_ID_SIZE
    ];

    size_t object_count;

    object_service_summary_t objects[
        OBJECT_SERVICE_MAX_OBJECTS
    ];

} object_service_event_t;


typedef void (*object_service_event_callback_t)(
    const object_service_event_t *event,
    void *user_context
);


typedef void (*object_service_event_observer_t)(
    const object_service_event_t *event,
    void *user_context
);


esp_err_t object_service_init(void);


void object_service_shutdown(void);


esp_err_t object_service_set_event_callback(
    object_service_event_callback_t callback,
    void *user_context
);


esp_err_t object_service_set_event_observer(
    object_service_event_observer_t observer,
    void *user_context
);


esp_err_t object_service_get_active(
    object_service_summary_t *object
);


esp_err_t object_service_create_async(void);


esp_err_t object_service_list_async(void);


esp_err_t object_service_set_active_async(
    const char *object_id
);


esp_err_t object_service_delete_async(
    const char *object_id
);


bool object_service_is_busy(void);
