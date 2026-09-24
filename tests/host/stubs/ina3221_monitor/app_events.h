#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    APP_EVENT_POWER_BATTERY_ALERT = 1,
} app_event_type_t;

#define APP_POWER_ALERT_LOW              1
#define APP_POWER_ALERT_CRITICAL_GRACE   2
#define APP_POWER_ALERT_CANCELLED        3
#define APP_POWER_ALERT_SHUTDOWN         4

esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value);
