#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void *esp_event_loop_handle_t;
typedef void (*esp_event_handler_t)(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);

#define WIFI_EVENT ((esp_event_base_t)"WIFI_EVENT")
#define IP_EVENT   ((esp_event_base_t)"IP_EVENT")

#define ESP_EVENT_ANY_ID                 (-1)
#define WIFI_EVENT_STA_START             1
#define WIFI_EVENT_STA_DISCONNECTED      2
#define WIFI_EVENT_AP_STACONNECTED       3
#define WIFI_EVENT_AP_STADISCONNECTED    4
#define IP_EVENT_STA_GOT_IP              5
#define IP_EVENT_AP_STAIPASSIGNED        6

esp_err_t esp_event_handler_register(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t handler,
    void *context
);

esp_err_t esp_event_handler_unregister(
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t handler
);
