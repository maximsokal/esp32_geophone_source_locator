#pragma once

#include <stdint.h>

#include "esp_err.h"


typedef enum
{
    APP_EVENT_KEYPAD = 0,

    APP_EVENT_WIFI_SETUP_CLIENT_READY,
    APP_EVENT_WIFI_SETUP_PAGE_OPENED,
    APP_EVENT_WIFI_CREDENTIALS_SAVED,
    APP_EVENT_WIFI_STA_CONNECTED,
    APP_EVENT_WIFI_STA_DISCONNECTED,

} app_event_type_t;


typedef struct
{
    app_event_type_t type;
    int32_t value;

} app_event_t;


esp_err_t app_event_bus_init(void);

esp_err_t app_event_bus_publish(
    app_event_type_t type,
    int32_t value
);

esp_err_t app_event_bus_receive(
    app_event_t *event
);

esp_err_t app_event_bus_receive_timeout(
    app_event_t *event,
    uint32_t timeout_ms
);
