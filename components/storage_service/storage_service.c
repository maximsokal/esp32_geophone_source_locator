#include "storage_service.h"

#include <stdbool.h>

#include "app_task_config.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage_service_internal.h"


static const char *TAG =
    "STORAGE_SERVICE";


#define STORAGE_SERVICE_QUEUE_LENGTH      1


static QueueHandle_t s_queue =
    NULL;

static SemaphoreHandle_t s_command_gate =
    NULL;

static TaskHandle_t s_task =
    NULL;

static storage_service_event_callback_t
    s_callback =
        NULL;

static void *s_callback_context =
    NULL;

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


static void storage_service_release_command_gate(void)
{
    if (s_command_gate != NULL) {
        (void)xSemaphoreGive(
            s_command_gate
        );
    }
}


static void storage_service_task(
    void *argument
)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Worker started core=%d",
        APP_TASK_CORE_SERVICES
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
         * Keep the gate held through the callback so the legacy
         * rule remains intact: another storage command is not
         * accepted until the current operation and its UI callback
         * have completed.
         */
        storage_service_release_command_gate();
    }

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
        s_queue == NULL ||
        s_command_gate == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * The binary semaphore makes "check + mark busy" atomic
     * across both CPU cores. A volatile bool did not.
     */
    if (
        xSemaphoreTake(
            s_command_gate,
            0
        ) != pdTRUE
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        xQueueSend(
            s_queue,
            &command,
            0
        ) != pdTRUE
    ) {
        storage_service_release_command_gate();

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

    s_command_gate =
        xSemaphoreCreateBinary();

    if (s_command_gate == NULL) {
        vQueueDelete(
            s_queue
        );

        s_queue =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    (void)xSemaphoreGive(
        s_command_gate
    );

    BaseType_t result =
        xTaskCreatePinnedToCore(
            storage_service_task,
            "storage_service",
            APP_TASK_STACK_STORAGE_SERVICE,
            NULL,
            APP_TASK_PRIORITY_STORAGE_SERVICE,
            &s_task,
            APP_TASK_CORE_SERVICES
        );

    if (result != pdPASS) {
        vSemaphoreDelete(
            s_command_gate
        );

        s_command_gate =
            NULL;

        vQueueDelete(
            s_queue
        );

        s_queue =
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


void storage_service_shutdown(void)
{
    if (!s_initialized) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );

    if (storage_service_is_busy()) {
        ESP_LOGI(
            TAG,
            "Waiting for active storage operation"
        );
    }

    storage_command_t command =
        STORAGE_COMMAND_STOP;

    if (s_queue != NULL) {
        (void)xQueueSend(
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

    if (s_command_gate != NULL) {
        vSemaphoreDelete(
            s_command_gate
        );

        s_command_gate =
            NULL;
    }

    s_callback =
        NULL;

    s_callback_context =
        NULL;

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
    if (
        !s_initialized ||
        s_command_gate == NULL
    ) {
        return false;
    }

    return
        uxSemaphoreGetCount(
            s_command_gate
        ) == 0;
}
