#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "keypad.h"

#include "ui_screen.h"


esp_err_t ui_status_bar_draw(void);


esp_err_t ui_common_begin_screen(
    const char *title
);


esp_err_t ui_common_draw_footer(
    const char *text
);


esp_err_t ui_common_draw_box(
    int x,
    int y,
    int width,
    int height,
    uint16_t color
);


esp_err_t ui_home_draw(
    size_t selected_item
);


esp_err_t ui_home_update_selection(
    size_t previous_item,
    size_t selected_item
);


esp_err_t ui_home_controller_open(void);


esp_err_t ui_home_controller_draw(void);


esp_err_t ui_home_controller_handle_key(
    keypad_key_t key
);


esp_err_t ui_objects_open(void);


esp_err_t ui_objects_draw(void);


esp_err_t ui_objects_handle_key(
    keypad_key_t key
);


esp_err_t ui_storage_open(void);


esp_err_t ui_storage_draw(void);


esp_err_t ui_storage_handle_key(
    keypad_key_t key
);


esp_err_t ui_measurements_draw(void);


esp_err_t ui_wifi_draw(void);


esp_err_t ui_system_draw(void);
