#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "keypad.h"


#define UI_MENU_ITEM_COUNT 5


typedef enum
{
    UI_SCREEN_HOME = 0,
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM

} ui_screen_t;


/*
 * Common UI elements.
 */
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


/*
 * Home screen.
 */
esp_err_t ui_home_draw(
    size_t selected_item
);


esp_err_t ui_home_update_selection(
    size_t previous_item,
    size_t selected_item
);


/*
 * Objects screen.
 */
esp_err_t ui_objects_open(void);


esp_err_t ui_objects_draw(void);


esp_err_t ui_objects_handle_key(
    keypad_key_t key
);


/*
 * Storage screen.
 */
esp_err_t ui_storage_open(void);


esp_err_t ui_storage_draw(void);


esp_err_t ui_storage_handle_key(
    keypad_key_t key
);


/*
 * Application screens.
 */
esp_err_t ui_measurements_draw(void);


esp_err_t ui_wifi_draw(void);


esp_err_t ui_system_draw(void);