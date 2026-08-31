#pragma once

#include <stdint.h>

#include "esp_err.h"


#define APP_TLS_VERIFY_SECURE_CONTEXT  (1U << 0)
#define APP_TLS_VERIFY_CAMERA          (1U << 1)
#define APP_TLS_VERIFY_XR_API          (1U << 2)
#define APP_TLS_VERIFY_IMMERSIVE_AR    (1U << 3)

#define APP_TLS_VERIFY_REQUIRED_MASK   \
    (APP_TLS_VERIFY_SECURE_CONTEXT |   \
     APP_TLS_VERIFY_CAMERA |           \
     APP_TLS_VERIFY_XR_API |           \
     APP_TLS_VERIFY_IMMERSIVE_AR)


typedef enum
{
    APP_EVENT_KEYPAD = 0,

    APP_EVENT_WIFI_SETUP_CLIENT_READY,
    APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED,
    APP_EVENT_WIFI_SETUP_PAGE_OPENED,
    APP_EVENT_WIFI_CREDENTIALS_SAVED,
    APP_EVENT_WIFI_STA_CONNECTING,
    APP_EVENT_WIFI_STA_CONNECTED,
    APP_EVENT_WIFI_STA_DISCONNECTED,

    APP_EVENT_TLS_CA_DOWNLOADED,
    APP_EVENT_TLS_CA_INSTALL_CONFIRMED,
    APP_EVENT_TLS_VERIFY_READY,
    APP_EVENT_TLS_VERIFY_RESULT,

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
