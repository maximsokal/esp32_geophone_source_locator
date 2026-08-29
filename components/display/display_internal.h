#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


esp_err_t display_driver_init(void);

void display_driver_deinit(void);

bool display_driver_is_initialized(void);

esp_lcd_panel_io_handle_t display_driver_io_handle(void);

esp_lcd_panel_handle_t display_driver_panel_handle(void);

esp_err_t display_panel_configure(
    esp_lcd_panel_handle_t panel_handle
);
