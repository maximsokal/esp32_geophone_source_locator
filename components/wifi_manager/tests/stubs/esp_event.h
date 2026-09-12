#pragma once

typedef void *esp_event_loop_handle_t;
typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(base) extern esp_event_base_t base
#define ESP_EVENT_ANY_ID (-1)
