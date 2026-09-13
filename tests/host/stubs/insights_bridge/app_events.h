#pragma once
#include "esp_event.h"
extern const char *APP_EVENT;
#define APP_EVENT_WIFI_STA_CONNECTED 7
esp_event_loop_handle_t app_events_get_loop(void);
