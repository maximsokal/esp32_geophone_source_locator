#pragma once

#include "esp_err.h"

#include "ui_screen.h"


esp_err_t ui_navigation_open_screen(
    ui_screen_t screen
);


esp_err_t ui_navigation_open_selected_home(void);


esp_err_t ui_navigation_return_home(void);
