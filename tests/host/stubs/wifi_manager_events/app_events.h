#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

typedef enum
{
    APP_EVENT_WIFI_SETUP_CLIENT_READY = 1,
    APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED,
    APP_EVENT_WIFI_STA_CONNECTED,
    APP_EVENT_WIFI_STA_DISCONNECTED,
} app_event_type_t;

esp_err_t app_events_post(app_event_type_t type, int32_t value);
