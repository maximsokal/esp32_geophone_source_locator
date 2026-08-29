#include "display_internal.h"

#include "board_config.h"
#include "spi_bus.h"

#include "esp_lcd_ili9341.h"
#include "esp_log.h"


static const char *TAG =
    "DISPLAY_DRIVER";


static esp_lcd_panel_io_handle_t s_io_handle =
    NULL;

static esp_lcd_panel_handle_t s_panel_handle =
    NULL;


bool display_driver_is_initialized(void)
{
    return
        s_panel_handle != NULL &&
        s_io_handle != NULL;
}


esp_lcd_panel_io_handle_t display_driver_io_handle(void)
{
    return
        s_io_handle;
}


esp_lcd_panel_handle_t display_driver_panel_handle(void)
{
    return
        s_panel_handle;
}


static void display_driver_cleanup(void)
{
    if (s_panel_handle != NULL) {
        (void)esp_lcd_panel_del(
            s_panel_handle
        );

        s_panel_handle =
            NULL;
    }

    if (s_io_handle != NULL) {
        (void)esp_lcd_panel_io_del(
            s_io_handle
        );

        s_io_handle =
            NULL;
    }
}


esp_err_t display_driver_init(void)
{
    if (display_driver_is_initialized()) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Creating SPI LCD IO for LVGL"
    );

    const esp_lcd_panel_io_spi_config_t io_config =
        ILI9341_PANEL_IO_SPI_CONFIG(
            BOARD_TFT_CS_GPIO,
            BOARD_TFT_DC_GPIO,
            NULL,
            NULL
        );

    esp_err_t err =
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)
                app_spi_bus_get_host(),
            &io_config,
            &s_io_handle
        );

    if (err != ESP_OK) {
        display_driver_cleanup();

        return err;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num =
            BOARD_TFT_RESET_GPIO,

        .rgb_ele_order =
            LCD_RGB_ELEMENT_ORDER_RGB,

        .bits_per_pixel =
            16,
    };

    err =
        esp_lcd_new_panel_ili9341(
            s_io_handle,
            &panel_config,
            &s_panel_handle
        );

    if (err != ESP_OK) {
        display_driver_cleanup();

        return err;
    }

    err =
        display_panel_configure(
            s_panel_handle
        );

    if (err != ESP_OK) {
        display_driver_cleanup();

        return err;
    }

    return ESP_OK;
}


void display_driver_deinit(void)
{
    display_driver_cleanup();
}
