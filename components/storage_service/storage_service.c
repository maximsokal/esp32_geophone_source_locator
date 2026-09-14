#include "storage_service.h"

#include <stdbool.h>

#include "app_task_config.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_task_wdt.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
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

static storage_service_callback_slot_t s_callback_slot = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .callback = NULL,
    .context = NULL,
    .in_flight = 0U,
};

static bool s_initialized =
    false;

static storage_service_event_t s_event;


storage_service_callback_slot_t *storage_service_internal_callback_slot(void)
{
    return &s_callback_slot;
}


static unsigned storage_service_callback_in_flight(void)
{
    unsigned in_flight;

    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );
    in_flight = s_callback_slot.in_flight;
    taskEXIT_CRITICAL(
        &s_callback_slot.lock
    );

    return in_flight;
}


static void storage_service_wait_for_callback(void)
{
    while (
        storage_service_callback_in_flight() != 0U
    ) {
        /* Never block or yield while holding the SMP spinlock. */
        vTaskDelay(1);
    }
}


static void storage_service_notify(void)
{
    storage_service_event_callback_t callback =
        NULL;
    void *callback_context =
        NULL;

    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );

    if (s_callback_slot.callback != NULL) {
        callback = s_callback_slot.callback;
        callback_context = s_callback_slot.context;
        ++s_callback_slot.in_flight;
    }

    taskEXIT_CRITICAL(
        &s_callback_slot.lock
    );

    if (callback == NULL) {
        return;
    }

    callback(
        &s_event,
        callback_context
    );

    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );
    --s_callback_slot.in_flight;
    taskEXIT_CRITICAL(
        &s_callback_slot.lock
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


static bool storage_service_watchdog_begin(void)
{
#ifdef ESP_PLATFORM
    const esp_err_t err =
        esp_task_wdt_add(NULL);

    if (err == ESP_OK) {
        return true;
    }

    ESP_LOGW(
        TAG,
        "TWDT subscribe failed: %s",
        esp_err_to_name(err)
    );
#endif

    return false;
}


static void storage_service_watchdog_end(
    bool subscribed
)
{
#ifdef ESP_PLATFORM
    if (!subscribed) {
        return;
    }

    const esp_err_t err =
        esp_task_wdt_delete(NULL);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "TWDT unsubscribe failed: %s",
            esp_err_to_name(err)
        );
    }
#else
    (void)subscribed;
#endif
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

        /*
         * Subscribe only while real storage work is active. Subscribing the
         * worker for its entire lifetime would turn the healthy blocking
         * xQueueReceive(portMAX_DELAY) idle state into a false TWDT timeout.
         * Progress checkpoints live inside the long filesystem traversal, so
         * a call that actually hangs cannot keep feeding the watchdog.
         */
        const bool watchdog_subscribed =
            storage_service_watchdog_begin();

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

        storage_service_watchdog_end(
            watchdog_subscribed
        );

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

    vTaskDeleteWithCaps(NULL);
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

    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );
    s_callback_slot.callback = NULL;
    s_callback_slot.context = NULL;
    s_callback_slot.in_flight = 0U;
    taskEXIT_CRITICAL(
        &s_callback_slot.lock
    );

    BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(
            storage_service_task,
            "storage_service",
            APP_TASK_STACK_STORAGE_SERVICE,
            NULL,
            APP_TASK_PRIORITY_STORAGE_SERVICE,
            &s_task,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
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
        "Initialized core=%d stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        APP_TASK_STACK_STORAGE_SERVICE
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

    /* Close the callback slot and drain any invocation already in flight. */
    (void)storage_service_set_event_callback(
        NULL,
        NULL
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

    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );
    s_callback_slot.callback = NULL;
    s_callback_slot.context = NULL;
    s_callback_slot.in_flight = 0U;
    taskEXIT_CRITICAL(
        &s_callback_slot.lock
    );

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

    /*
     * Close the old slot first. A notify path that already snapped the old
     * callback incremented in_flight under this same lock, so the wait below
     * is a lifetime barrier for the old callback/context.
     */
    taskENTER_CRITICAL(
        &s_callback_slot.lock
    );
    s_callback_slot.callback = NULL;
    s_callback_slot.context = NULL;
    taskEXIT_CRITICAL(
        &s_callback_slot.lock
    );

    storage_service_wait_for_callback();

    if (callback != NULL) {
        taskENTER_CRITICAL(
            &s_callback_slot.lock
        );
        s_callback_slot.context = user_context;
        s_callback_slot.callback = callback;
        taskEXIT_CRITICAL(
            &s_callback_slot.lock
        );
    }

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
