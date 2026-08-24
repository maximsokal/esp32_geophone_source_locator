#include "storage_service.h"

#include <stdbool.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "storage_service_internal.h"


static const char *TAG =
    "STORAGE_SERVICE";


#define STORAGE_SERVICE_QUEUE_LENGTH      1
#define STORAGE_SERVICE_TASK_STACK_SIZE   8192
#define STORAGE_SERVICE_TASK_PRIORITY     3


static QueueHandle_t s_queue =
    NULL;


static TaskHandle_t s_task =
    NULL;


static storage_service_event_callback_t
    s_callback =
        NULL;


static void *s_callback_context =
    NULL;


static volatile bool s_busy =
    false;


static bool s_initialized =
    false;


static storage_service_event_t s_event;


static void storage_service_notify(void)
{
    if (s_callback == NULL) {
        return;
    }

    s_callback(
        &s_event,
        s_callback_context
    );
}


static void storage_service_task(
    void *argument
)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Worker started"
    );

    while (true) {
        storage_command_t command;

        if (
            xQueueReceive(
                s_queue,
                &command,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }

        if (
            command ==
            STORAGE_COMMAND_STOP
        ) {
            break;
        }

        bool notify =
            false;

        switch (command) {
            case STORAGE_COMMAND_INFO:
                notify =
                    storage_service_process_info(
                        &s_event
                    );
                break;

            case STORAGE_COMMAND_ERASE_DATA:
                notify =
                    storage_service_process_erase(
                        &s_event
                    );
                break;

            case STORAGE_COMMAND_STOP:
            default:
                break;
        }

        if (notify) {
            storage_service_notify();
        }

        /*
         * UI callbacks finish before another storage command
         * is accepted.
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

    s_task =
        NULL;

    vTaskDelete(
        NULL
    );
}


static esp_err_t storage_service_submit(
    storage_command_t command
)
{
    if (
        !s_initialized ||
        s_queue == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }

    s_busy =
        true;

    if (
        xQueueSend(
            s_queue,
            &command,
            0
        ) != pdTRUE
    ) {
        s_busy =
            false;

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


esp_err_t storage_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_queue =
        xQueueCreate(
            STORAGE_SERVICE_QUEUE_LENGTH,
            sizeof(storage_command_t)
        );

    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result =
        xTaskCreate(
            storage_service_task,
            "storage_service",
            STORAGE_SERVICE_TASK_STACK_SIZE,
            NULL,
            STORAGE_SERVICE_TASK_PRIORITY,
            &s_task
        );

    if (result != pdPASS) {
        vQueueDelete(
            s_queue
        );

        s_queue =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    s_busy =
        false;

    s_initialized =
        true;

    ESP_LOGI(
        TAG,
        "Initialized"
    );

    return ESP_OK;
}


void storage_service_shutdown(void)
{
    if (!s_initialized) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );

    if (s_busy) {
        ESP_LOGI(
            TAG,
            "Waiting for active storage operation"
        );
    }

    storage_command_t command =
        STORAGE_COMMAND_STOP;

    if (s_queue != NULL) {
        xQueueSend(
            s_queue,
            &command,
            portMAX_DELAY
        );
    }

    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    if (s_queue != NULL) {
        vQueueDelete(
            s_queue
        );

        s_queue =
            NULL;
    }

    s_callback =
        NULL;

    s_callback_context =
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


esp_err_t storage_service_set_event_callback(
    storage_service_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_callback =
        callback;

    s_callback_context =
        user_context;

    return ESP_OK;
}


esp_err_t storage_service_info_async(void)
{
    return storage_service_submit(
        STORAGE_COMMAND_INFO
    );
}


esp_err_t storage_service_erase_data_async(void)
{
    return storage_service_submit(
        STORAGE_COMMAND_ERASE_DATA
    );
}


bool storage_service_is_busy(void)
{
    return s_busy;
}
