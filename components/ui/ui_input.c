#include "ui.h"

#include "app_event_bus.h"

#include "esp_log.h"


static const char *TAG =
    "UI";


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
)
{
    (void)user_context;

    ESP_LOGI(
        TAG,
        "UI key queued: %s",
        keypad_key_name(key)
    );

    esp_err_t err =
        app_event_bus_publish(
            APP_EVENT_KEYPAD,
            (int32_t)key
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to queue key: %s",
            esp_err_to_name(err)
        );
    }
}
