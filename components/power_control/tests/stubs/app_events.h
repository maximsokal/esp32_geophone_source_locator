#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    APP_EVENT_POWER_SHUTDOWN_REQUESTED = 0,
} app_event_type_t;

esp_err_t app_events_post(app_event_type_t type, int32_t value);
esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value);
