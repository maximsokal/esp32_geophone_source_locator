#include "object_service_internal.h"

#include "esp_log.h"

#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"


static const char *TAG =
    "OBJECT_SERVICE";


void object_service_internal_worker(
    void *arg
)
{
    (void)arg;

    object_service_state_t *state =
        object_service_internal_state();

    ESP_LOGI(
        TAG,
        "Worker started"
    );

    while (true) {
        object_service_command_t command;

        if (
            xQueueReceive(
                state->command_queue,
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

        object_service_internal_process(
            &command
        );

        state->busy =
            false;
    }

    state->busy =
        false;

    ESP_LOGI(
        TAG,
        "Worker stopped"
    );

    state->worker_task =
        NULL;

    vTaskDeleteWithCaps(NULL);
}
