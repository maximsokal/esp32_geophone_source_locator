#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_loop_handle_t;
typedef void (*esp_event_handler_t)(void *, esp_event_base_t, int32_t, void *);

#define ESP_EVENT_ANY_ID (-1)

esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg);
esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler);
