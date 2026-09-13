#include "object_service.h"

#include "object_service_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"


static const char *TAG =
    "OBJECT_SERVICE";


void object_service_shutdown(void)
{
    object_service_state_t *state =
        object_service_internal_state();

    if (!state->initialized) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Shutdown requested"
    );

    /*
     * Close both delivery slots and wait for callbacks that already snapped
     * them before stopping the worker. This makes shutdown safe even when a
     * consumer destroys its callback context immediately afterwards.
     */
    (void)object_service_set_event_callback(
        NULL,
        NULL
    );
    (void)object_service_set_event_observer(
        NULL,
        NULL
    );

    object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_STOP,
    };

    if (state->command_queue != NULL) {
        xQueueSend(
            state->command_queue,
            &command,
            portMAX_DELAY
        );
    }

    while (
        state->worker_task !=
        NULL
    ) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    if (state->command_queue != NULL) {
        vQueueDelete(
            state->command_queue
        );

        state->command_queue =
            NULL;
    }

    taskENTER_CRITICAL(
        &state->event_lock
    );
    state->event_callback = NULL;
    state->event_user_context = NULL;
    state->event_callback_in_flight = 0U;
    state->event_observer = NULL;
    state->event_observer_context = NULL;
    state->event_observer_in_flight = 0U;
    taskEXIT_CRITICAL(
        &state->event_lock
    );

    atomic_store_explicit(
        &state->busy,
        false,
        memory_order_release
    );

    state->initialized =
        false;

    ESP_LOGI(
        TAG,
        "Shutdown complete"
    );
}
