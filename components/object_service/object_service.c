#include "object_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


static const char *TAG =
    "OBJECT_SERVICE";


#define OBJECT_SERVICE_QUEUE_LENGTH       1
#define OBJECT_SERVICE_TASK_STACK_SIZE    8192
#define OBJECT_SERVICE_TASK_PRIORITY      3


typedef enum
{
    OBJECT_SERVICE_COMMAND_CREATE = 0,
    OBJECT_SERVICE_COMMAND_LIST,
    OBJECT_SERVICE_COMMAND_SET_ACTIVE,
    OBJECT_SERVICE_COMMAND_STOP

} object_service_command_type_t;


typedef struct
{
    object_service_command_type_t type;

    char object_id[
        OBJECT_SERVICE_OBJECT_ID_SIZE
    ];

} object_service_command_t;


static QueueHandle_t s_command_queue =
    NULL;


static TaskHandle_t s_worker_task =
    NULL;


static object_service_event_callback_t
    s_event_callback =
        NULL;


static void *s_event_user_context =
    NULL;


static volatile bool s_busy =
    false;


static bool s_initialized =
    false;


/*
 * Event storage is static instead of being allocated
 * on the worker task stack.
 *
 * LIST may contain up to OBJECT_STORE_MAX_OBJECTS
 * summaries, so keeping the event in BSS saves stack.
 */
static object_service_event_t s_event;


static void object_service_notify(
    const object_service_event_t *event
)
{
    if (
        s_event_callback ==
        NULL
    ) {
        return;
    }


    s_event_callback(
        event,
        s_event_user_context
    );
}


static void object_service_prepare_event(void)
{
    memset(
        &s_event,
        0,
        sizeof(s_event)
    );


    s_event.result =
        ESP_OK;
}


static void object_service_process_create(void)
{
    object_service_prepare_event();


    s_event.result =
        object_store_create(
            &s_event.object
        );


    if (
        s_event.result ==
        ESP_OK
    ) {
        s_event.type =
            OBJECT_SERVICE_EVENT_CREATE_DONE;


        ESP_LOGI(
            TAG,
            "Create completed: %s",
            s_event.object.id
        );
    }
    else {
        s_event.type =
            OBJECT_SERVICE_EVENT_CREATE_FAILED;


        ESP_LOGE(
            TAG,
            "Create failed: %s",
            esp_err_to_name(
                s_event.result
            )
        );
    }


    object_service_notify(
        &s_event
    );
}


static void object_service_process_list(void)
{
    object_service_prepare_event();


    s_event.result =
        object_store_list(
            s_event.objects,
            OBJECT_STORE_MAX_OBJECTS,
            &s_event.object_count
        );


    if (
        s_event.result ==
        ESP_OK
    ) {
        s_event.type =
            OBJECT_SERVICE_EVENT_LIST_DONE;


        ESP_LOGI(
            TAG,
            "List completed: %u object(s)",
            (unsigned)s_event.object_count
        );
    }
    else {
        s_event.type =
            OBJECT_SERVICE_EVENT_LIST_FAILED;


        ESP_LOGE(
            TAG,
            "List failed: %s",
            esp_err_to_name(
                s_event.result
            )
        );
    }


    object_service_notify(
        &s_event
    );
}


static void object_service_process_set_active(
    const char *object_id
)
{
    object_service_prepare_event();


    size_t id_length =
        strlen(
            object_id
        );


    if (
        id_length >=
        sizeof(s_event.object_id)
    ) {
        id_length =
            sizeof(s_event.object_id) -
            1;
    }


    memcpy(
        s_event.object_id,
        object_id,
        id_length
    );


    s_event.object_id[
        id_length
    ] = '\0';


    s_event.result =
        object_store_set_active(
            object_id
        );


    if (
        s_event.result ==
        ESP_OK
    ) {
        s_event.type =
            OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE;


        ESP_LOGI(
            TAG,
            "Set active completed: %s",
            object_id
        );
    }
    else {
        s_event.type =
            OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED;


        ESP_LOGE(
            TAG,
            "Set active failed for %s: %s",
            object_id,
            esp_err_to_name(
                s_event.result
            )
        );
    }


    object_service_notify(
        &s_event
    );
}


static void object_service_worker_task(
    void *arg
)
{
    (void)arg;


    ESP_LOGI(
        TAG,
        "Worker started"
    );


    while (true) {

        object_service_command_t command;


        if (
            xQueueReceive(
                s_command_queue,
                &command,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }


        if (
            command.type ==
            OBJECT_SERVICE_COMMAND_STOP
        ) {
            break;
        }


        switch (
            command.type
        ) {

            case OBJECT_SERVICE_COMMAND_CREATE:

                object_service_process_create();

                break;


            case OBJECT_SERVICE_COMMAND_LIST:

                object_service_process_list();

                break;


            case OBJECT_SERVICE_COMMAND_SET_ACTIVE:

                object_service_process_set_active(
                    command.object_id
                );

                break;


            case OBJECT_SERVICE_COMMAND_STOP:
            default:

                break;
        }


        /*
         * Keep the service busy while the completion
         * callback redraws the UI.
         *
         * This prevents keypad-driven drawing from using
         * the display at the same time as this task.
         */
        s_busy =
            false;
    }


    s_busy =
        false;


    ESP_LOGI(
        TAG,
        "Worker stopped"
    );


    s_worker_task =
        NULL;


    vTaskDelete(
        NULL
    );
}


static esp_err_t object_service_submit(
    const object_service_command_t *command
)
{
    if (
        !s_initialized ||
        s_command_queue == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Busy is set before placing the command into the queue.
     * Keypad/UI code can immediately see that storage work
     * is in progress.
     */
    s_busy =
        true;


    if (
        xQueueSend(
            s_command_queue,
            command,
            0
        ) != pdTRUE
    ) {
        s_busy =
            false;


        return ESP_ERR_TIMEOUT;
    }


    return ESP_OK;
}


esp_err_t object_service_init(void)
{
    if (s_initialized) {

        ESP_LOGW(
            TAG,
            "Object service already initialized"
        );


        return ESP_OK;
    }


    s_command_queue =
        xQueueCreate(
            OBJECT_SERVICE_QUEUE_LENGTH,
            sizeof(
                object_service_command_t
            )
        );


    if (
        s_command_queue ==
        NULL
    ) {
        return ESP_ERR_NO_MEM;
    }


    s_busy =
        false;


    BaseType_t result =
        xTaskCreate(
            object_service_worker_task,
            "object_service",
            OBJECT_SERVICE_TASK_STACK_SIZE,
            NULL,
            OBJECT_SERVICE_TASK_PRIORITY,
            &s_worker_task
        );


    if (
        result !=
        pdPASS
    ) {
        vQueueDelete(
            s_command_queue
        );


        s_command_queue =
            NULL;


        return ESP_ERR_NO_MEM;
    }


    s_initialized =
        true;


    ESP_LOGI(
        TAG,
        "Initialized"
    );


    return ESP_OK;
}


void object_service_shutdown(void)
{
    if (
        !s_initialized
    ) {
        return;
    }


    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );


    object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_STOP,
    };


    /*
     * If an SD operation is currently executing, STOP waits
     * in the queue. The active write completes first.
     */
    if (
        s_command_queue !=
        NULL
    ) {
        xQueueSend(
            s_command_queue,
            &command,
            portMAX_DELAY
        );
    }


    while (
        s_worker_task !=
        NULL
    ) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }


    if (
        s_command_queue !=
        NULL
    ) {
        vQueueDelete(
            s_command_queue
        );


        s_command_queue =
            NULL;
    }


    s_event_callback =
        NULL;


    s_event_user_context =
        NULL;


    s_busy =
        false;


    s_initialized =
        false;


    ESP_LOGI(
        TAG,
        "Shutdown complete"
    );
}


esp_err_t object_service_set_event_callback(
    object_service_event_callback_t callback,
    void *user_context
)
{
    if (
        !s_initialized
    ) {
        return ESP_ERR_INVALID_STATE;
    }


    s_event_callback =
        callback;


    s_event_user_context =
        user_context;


    return ESP_OK;
}


esp_err_t object_service_create_async(void)
{
    const object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_CREATE,
    };


    return object_service_submit(
        &command
    );
}


esp_err_t object_service_list_async(void)
{
    const object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_LIST,
    };


    return object_service_submit(
        &command
    );
}


esp_err_t object_service_set_active_async(
    const char *object_id
)
{
    if (
        object_id ==
        NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    const size_t id_length =
        strlen(
            object_id
        );


    if (
        id_length == 0 ||
        id_length >=
            OBJECT_SERVICE_OBJECT_ID_SIZE
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_SET_ACTIVE,
    };


    memcpy(
        command.object_id,
        object_id,
        id_length + 1
    );


    return object_service_submit(
        &command
    );
}


bool object_service_is_busy(void)
{
    return s_busy;
}