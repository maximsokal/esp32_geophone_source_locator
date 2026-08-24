#pragma once

#include "esp_err.h"

#include "keypad.h"

#include "ui_screen.h"


esp_err_t ui_screen_open(
    ui_screen_t screen
);


esp_err_t ui_screen_draw(
    ui_screen_t screen
);


esp_err_t ui_screen_handle_key(
    ui_screen_t screen,
    keypad_key_t key
);
