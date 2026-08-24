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
     * Preserve the user-verified physical orientation:
     * native 240x320 -> landscape 320x240.
     */
    err =
        esp_lcd_panel_swap_xy(
            panel_handle,
            true
        );

    if (err != ESP_OK) {
        return err;
    }

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
        "ILI9341 initialized: %dx%d landscape RGB565",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    );

    return ESP_OK;
}
