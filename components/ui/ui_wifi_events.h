#pragma once

#include "app_event_bus.h"
#include "esp_err.h"


esp_err_t ui_wifi_events_handle(
    app_event_type_t type
);
