#pragma once

#include <stdbool.h>
#include <stdatomic.h>

#include "object_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


#define OBJECT_SERVICE_QUEUE_LENGTH       1


typedef enum
{
    OBJECT_SERVICE_COMMAND_CREATE = 0,
    OBJECT_SERVICE_COMMAND_LIST,
    OBJECT_SERVICE_COMMAND_SET_ACTIVE,
    OBJECT_SERVICE_COMMAND_DELETE,
    OBJECT_SERVICE_COMMAND_STOP

} object_service_command_type_t;


typedef struct
{
    object_service_command_type_t type;

    char object_id[
        OBJECT_SERVICE_OBJECT_ID_SIZE
    ];

} object_service_command_t;


typedef struct
{
    QueueHandle_t command_queue;

    TaskHandle_t worker_task;

    /*
     * Event callback slots are shared between the service worker and lifecycle
     * owners on another core. Keep this spinlock limited to slot snapshot and
     * in-flight accounting only; callbacks and waits always run outside it.
     */
    portMUX_TYPE event_lock;

    object_service_event_callback_t event_callback;

    void *event_user_context;

    unsigned event_callback_in_flight;

    object_service_event_observer_t event_observer;

    void *event_observer_context;

    unsigned event_observer_in_flight;

    atomic_bool busy;

    bool initialized;

    object_service_event_t event;

} object_service_state_t;


object_service_state_t *object_service_internal_state(void);


esp_err_t object_service_internal_submit(
    const object_service_command_t *command
);


void object_service_internal_worker(
    void *arg
);


void object_service_internal_process(
    const object_service_command_t *command
);


void object_service_internal_process_mutation(
    const object_service_command_t *command
);


void object_service_internal_notify(
    object_service_state_t *state
);
