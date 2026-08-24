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

    state->event_callback =
        NULL;

    state->event_user_context =
        NULL;

    state->busy =
        false;

    state->initialized =
        false;

    ESP_LOGI(
        TAG,
        "Shutdown complete"
    );
}
