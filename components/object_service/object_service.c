#include "object_service.h"

#include "object_service_internal.h"

#include "object_store.h"

#include "esp_log.h"

#include "freertos/queue.h"


static const char *TAG =
    "OBJECT_SERVICE";


esp_err_t object_service_init(void)
{
    object_service_state_t *state =
        object_service_internal_state();

    if (state->initialized) {
        return ESP_OK;
    }

    state->command_queue =
        xQueueCreate(
            OBJECT_SERVICE_QUEUE_LENGTH,
            sizeof(object_service_command_t)
        );

    if (state->command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    state->busy =
        false;

    BaseType_t result =
        xTaskCreate(
            object_service_internal_worker,
            "object_service",
            OBJECT_SERVICE_TASK_STACK_SIZE,
            NULL,
            OBJECT_SERVICE_TASK_PRIORITY,
            &state->worker_task
        );

    if (result != pdPASS) {
        vQueueDelete(
            state->command_queue
        );

        state->command_queue =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    state->initialized =
        true;

    ESP_LOGI(
        TAG,
        "Initialized"
    );

    return ESP_OK;
}


esp_err_t object_service_set_event_callback(
    object_service_event_callback_t callback,
    void *user_context
)
{
    object_service_state_t *state =
        object_service_internal_state();

    if (!state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    state->event_callback =
        callback;

    state->event_user_context =
        user_context;

    return ESP_OK;
}


esp_err_t object_service_get_active(
    object_service_summary_t *object
)
{
    if (object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        !object_service_internal_state()
            ->initialized
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        object_store_get_active(
            object
        );
}


bool object_service_is_busy(void)
{
    return
        object_service_internal_state()
            ->busy;
}
