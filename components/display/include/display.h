#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240


/*
 * Initialize only the physical ILI9341/esp_lcd layer.
 * Application rendering is owned by LVGL in the ui component.
 */
esp_err_t display_init(void);


bool display_is_ready(void);


esp_lcd_panel_io_handle_t display_io_handle(void);


esp_lcd_panel_handle_t display_panel_handle(void);


esp_err_t display_set_enabled(
    bool enabled
);


void display_deinit(void);
