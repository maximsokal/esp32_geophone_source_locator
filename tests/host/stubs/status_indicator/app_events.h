#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

extern const char *APP_EVENT;

typedef enum {
    APP_EVENT_STATUS_WARNING = 10,
    APP_EVENT_STATUS_ERROR = 11,
    APP_EVENT_STATUS_CLEAR_NON_STICKY = 12
} app_event_type_t;

esp_event_loop_handle_t app_events_get_loop(void);
esp_err_t app_events_post(app_event_type_t type, int32_t value);
esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value);
