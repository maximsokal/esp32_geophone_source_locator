#pragma once

#include <stdbool.h>

#include "object_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


#define OBJECT_SERVICE_QUEUE_LENGTH       1
#define OBJECT_SERVICE_TASK_STACK_SIZE    8192
#define OBJECT_SERVICE_TASK_PRIORITY      3


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

    object_service_event_callback_t event_callback;

    void *event_user_context;

    volatile bool busy;

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
