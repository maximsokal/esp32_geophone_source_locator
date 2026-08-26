#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t ui_wifi_navigation_move(
    bool down
);

esp_err_t ui_wifi_navigation_select(void);
