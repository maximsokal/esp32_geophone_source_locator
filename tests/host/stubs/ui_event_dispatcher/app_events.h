#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef void *esp_event_loop_handle_t;
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

#define APP_EVENT ((esp_event_base_t)"APP_EVENT")
#define APP_EVENT_KEYPAD 1
#define ESP_EVENT_ANY_ID (-1)

esp_event_loop_handle_t app_events_get_loop(void);

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
