#include "app_events.h"

const char APP_EVENT_BASE[] = "APP_EVENT";

bool app_events_keypad_delivery_enabled(void)
{
    return true;
}


esp_err_t esp_event_post_to(
    esp_event_loop_handle_t event_loop,
    esp_event_base_t event_base,
    int32_t event_id,
    const void *event_data,
    size_t event_data_size,
    uint32_t ticks_to_wait
)
{
    (void)event_loop;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    (void)event_data_size;
    (void)ticks_to_wait;
    return ESP_OK;
}
