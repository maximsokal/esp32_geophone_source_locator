#include "display.h"

#include "app_display_config.h"
#include "display_internal.h"

#include "esp_log.h"


static const char *TAG =
    "DISPLAY";


esp_err_t display_init(void)
{
#if APP_DISPLAY_ENABLED
    if (display_is_ready()) {
        return ESP_OK;
    }

    return
        display_driver_init();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


bool display_is_ready(void)
{
    return
        display_driver_is_initialized();
}


esp_lcd_panel_io_handle_t display_io_handle(void)
{
    return
        display_driver_io_handle();
}


esp_lcd_panel_handle_t display_panel_handle(void)
{
    return
        display_driver_panel_handle();
}


esp_err_t display_set_enabled(
    bool enabled
)
{
    esp_lcd_panel_handle_t panel =
        display_panel_handle();

    if (panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        esp_lcd_panel_disp_on_off(
            panel,
            enabled
        );
}


void display_deinit(void)
{
    display_driver_deinit();

    ESP_LOGI(
        TAG,
        "Display deinitialized"
    );
}
