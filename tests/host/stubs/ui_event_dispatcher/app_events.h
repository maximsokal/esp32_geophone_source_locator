#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

extern const char APP_EVENT_BASE[];
#define APP_EVENT ((esp_event_base_t)APP_EVENT_BASE)
#define APP_EVENT_KEYPAD 1
#define ESP_EVENT_ANY_ID (-1)

esp_event_loop_handle_t app_events_get_loop(void);
bool app_events_keypad_delivery_enabled(void);

esp_err_t esp_event_handler_register_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg
);

esp_err_t esp_event_handler_unregister_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler
);
