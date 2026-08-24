#include "storage_service.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "object_service.h"
#include "object_store.h"
#include "power_control.h"
#include "sd_card.h"
#include "storage_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


static const char *TAG =
    "STORAGE_SERVICE";


#define STORAGE_SERVICE_QUEUE_LENGTH      1
#define STORAGE_SERVICE_TASK_STACK_SIZE   8192
#define STORAGE_SERVICE_TASK_PRIORITY     3


typedef enum
{
    STORAGE_COMMAND_INFO = 0,
    STORAGE_COMMAND_ERASE_DATA,
    STORAGE_COMMAND_STOP

} storage_command_t;


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


static void storage_service_process_info(void)
{
    memset(
        &s_event,
        0,
        sizeof(s_event)
    );

    s_event.result =
        sd_card_get_info(
            &s_event.info
        );

    if (s_event.result == ESP_OK) {
        s_event.type =
            STORAGE_SERVICE_EVENT_INFO_DONE;

        ESP_LOGI(
            TAG,
            "Card info completed"
        );
    }
    else {
        s_event.type =
            STORAGE_SERVICE_EVENT_INFO_FAILED;

        ESP_LOGE(
            TAG,
            "Card info failed: %s",
            esp_err_to_name(
                s_event.result
            )
        );
    }

    storage_service_notify();
}


static esp_err_t storage_service_erase_data(void)
{
    /*
     * Application data must not be deleted while an object
     * create/delete/select operation is still using storage.
     */
    if (
        object_service_is_busy()
    ) {
        ESP_LOGW(
            TAG,
            "Cannot erase data while object service is busy"
        );

        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        storage_manager_erase_application_data();

    if (err != ESP_OK) {
        return err;
    }

    /*
     * The active object marker was deleted together with
     * application data. Clear the in-memory object state too.
     */
    return object_store_reset();
}


static void storage_service_process_erase(void)
{
    memset(
        &s_event,
        0,
        sizeof(s_event)
    );

    s_event.result =
        storage_service_erase_data();

    if (s_event.result == ESP_OK) {
        s_event.type =
            STORAGE_SERVICE_EVENT_ERASE_DONE;

        ESP_LOGI(
            TAG,
            "Application data erase completed"
        );
    }
    else {
        s_event.type =
            STORAGE_SERVICE_EVENT_ERASE_FAILED;

        ESP_LOGE(
            TAG,
            "Application data erase failed: %s",
            esp_err_to_name(
                s_event.result
            )
        );
    }

    /*
     * If POWER was pressed during erase, do not spend more
     * time redrawing the STORAGE menu.
     *
     * The erase has already completed safely. The main task
     * can now continue the pending shutdown sequence.
     */
    if (
        power_control_is_shutdown_requested()
    ) {
        ESP_LOGI(
            TAG,
            "Shutdown pending after erase; skipping UI redraw"
        );

        return;
    }

    storage_service_notify();
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

        switch (command) {
            case STORAGE_COMMAND_INFO:
                storage_service_process_info();
                break;

            case STORAGE_COMMAND_ERASE_DATA:
                storage_service_process_erase();
                break;

            case STORAGE_COMMAND_STOP:
            default:
                break;
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