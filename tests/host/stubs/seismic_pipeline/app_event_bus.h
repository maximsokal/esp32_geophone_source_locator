#pragma once

#include "esp_err.h"

#define APP_EVENT_WIFI_CREDENTIALS_SAVED 1
esp_err_t app_event_bus_publish(int event, int value);
