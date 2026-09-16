#include "storage_service_internal.h"

#include "freertos/task.h"


static storage_service_content_observer_slot_t s_content_observer = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};


storage_service_content_observer_slot_t *
storage_service_internal_content_observer_slot(void)
{
    return &s_content_observer;
}


static unsigned storage_service_content_observer_in_flight(void)
{
    unsigned in_flight;
    taskENTER_CRITICAL(&s_content_observer.lock);
    in_flight = s_content_observer.in_flight;
    taskEXIT_CRITICAL(&s_content_observer.lock);
    return in_flight;
}


static void storage_service_wait_for_content_observer(void)
{
    while (storage_service_content_observer_in_flight() != 0U) {
        vTaskDelay(1U);
    }
}


esp_err_t storage_service_set_content_changed_observer(
    storage_service_content_changed_callback_t callback,
    void *user_context
)
{
    taskENTER_CRITICAL(&s_content_observer.lock);
    s_content_observer.callback = NULL;
    s_content_observer.context = NULL;
    taskEXIT_CRITICAL(&s_content_observer.lock);

    storage_service_wait_for_content_observer();

    if (callback != NULL) {
        taskENTER_CRITICAL(&s_content_observer.lock);
        s_content_observer.callback = callback;
        s_content_observer.context = user_context;
        taskEXIT_CRITICAL(&s_content_observer.lock);
    }

    return ESP_OK;
}


void storage_service_internal_notify_content_changed(void)
{
    storage_service_content_changed_callback_t callback = NULL;
    void *context = NULL;

    taskENTER_CRITICAL(&s_content_observer.lock);
    if (s_content_observer.callback != NULL) {
        callback = s_content_observer.callback;
        context = s_content_observer.context;
        ++s_content_observer.in_flight;
    }
    taskEXIT_CRITICAL(&s_content_observer.lock);

    if (callback == NULL) {
        return;
    }

    callback(context);

    taskENTER_CRITICAL(&s_content_observer.lock);
    if (s_content_observer.in_flight > 0U) {
        --s_content_observer.in_flight;
    }
    taskEXIT_CRITICAL(&s_content_observer.lock);
}


void storage_service_internal_clear_content_observer(void)
{
    (void)storage_service_set_content_changed_observer(NULL, NULL);
}
