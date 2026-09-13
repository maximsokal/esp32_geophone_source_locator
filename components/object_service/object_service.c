#include "object_service.h"

#include "object_service_internal.h"

#include "app_task_config.h"
#include "object_store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"


static const char *TAG =
    "OBJECT_SERVICE";


static unsigned object_service_event_slot_in_flight(
    object_service_state_t *state,
    bool observer
)
{
    unsigned in_flight;

    taskENTER_CRITICAL(
        &state->event_lock
    );

    in_flight = observer
        ? state->event_observer_in_flight
        : state->event_callback_in_flight;

    taskEXIT_CRITICAL(
        &state->event_lock
    );

    return in_flight;
}


static void object_service_wait_for_event_slot(
    object_service_state_t *state,
    bool observer
)
{
    while (
        object_service_event_slot_in_flight(
            state,
            observer
        ) != 0U
    ) {
        /* Never block or yield while holding the SMP spinlock. */
        vTaskDelay(1);
    }
}


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

    atomic_store_explicit(
        &state->busy,
        false,
        memory_order_release
    );

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

    BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(
            object_service_internal_worker,
            "object_service",
            APP_TASK_STACK_OBJECT_SERVICE,
            NULL,
            APP_TASK_PRIORITY_OBJECT_SERVICE,
            &state->worker_task,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
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
        "Initialized core=%d stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        APP_TASK_STACK_OBJECT_SERVICE
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

    /*
     * Close the old slot before waiting. Any worker invocation that already
     * snapped the callback incremented the in-flight counter under this same
     * lock, so returning from this function is a lifetime barrier for the old
     * callback/context.
     */
    taskENTER_CRITICAL(
        &state->event_lock
    );
    state->event_callback = NULL;
    state->event_user_context = NULL;
    taskEXIT_CRITICAL(
        &state->event_lock
    );

    object_service_wait_for_event_slot(
        state,
        false
    );

    if (callback != NULL) {
        taskENTER_CRITICAL(
            &state->event_lock
        );
        state->event_user_context = user_context;
        state->event_callback = callback;
        taskEXIT_CRITICAL(
            &state->event_lock
        );
    }

    return ESP_OK;
}


esp_err_t object_service_set_event_observer(
    object_service_event_observer_t observer,
    void *user_context
)
{
    object_service_state_t *state =
        object_service_internal_state();

    if (!state->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(
        &state->event_lock
    );
    state->event_observer = NULL;
    state->event_observer_context = NULL;
    taskEXIT_CRITICAL(
        &state->event_lock
    );

    object_service_wait_for_event_slot(
        state,
        true
    );

    if (observer != NULL) {
        taskENTER_CRITICAL(
            &state->event_lock
        );
        state->event_observer_context = user_context;
        state->event_observer = observer;
        taskEXIT_CRITICAL(
            &state->event_lock
        );
    }

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
    return atomic_load_explicit(
        &object_service_internal_state()->busy,
        memory_order_acquire
    );
}
