#pragma once

#include "app_event_bus.h"
#include "esp_err.h"
#include "keypad.h"


esp_err_t ui_event_dispatch_key(
    keypad_key_t key
);

esp_err_t ui_event_dispatch_app(
    app_event_type_t type
);
