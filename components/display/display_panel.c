#include "display_internal.h"

#include "display.h"

#include "esp_log.h"


static const char *TAG =
    "DISPLAY_PANEL";


esp_err_t display_panel_configure(
    esp_lcd_panel_handle_t panel_handle
)
{
    if (panel_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "Resetting display"
    );

    esp_err_t err =
        esp_lcd_panel_reset(
            panel_handle
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Initializing display"
    );

    err =
        esp_lcd_panel_init(
            panel_handle
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Keep the hardware layer in the panel's native orientation.
     * Stage 11D deliberately gives rotation ownership to
     * esp_lvgl_port so there is exactly one place that controls
     * portrait/landscape state.
     */
    err =
        esp_lcd_panel_mirror(
            panel_handle,
            false,
            false
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        esp_lcd_panel_disp_on_off(
            panel_handle,
            true
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "ILI9341 hardware ready: native 240x320 RGB565; orientation owned by LVGL"
    );

    return ESP_OK;
}
