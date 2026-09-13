#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_EVENT_KEYPAD = 0,
} app_event_type_t;

esp_err_t app_events_post(
    app_event_type_t type,
    int32_t value
);

static inline void app_events_set_keypad_delivery_enabled(bool enabled)
{
    (void)enabled;
}
