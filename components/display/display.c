#include "display.h"

#include <stdlib.h>

#include "board_config.h"
#include "spi_bus.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

#include "esp_log.h"

static const char *TAG = "DISPLAY";

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Creating SPI LCD IO");

    const esp_lcd_panel_io_spi_config_t io_config =
        ILI9341_PANEL_IO_SPI_CONFIG(
            BOARD_TFT_CS_GPIO,
            BOARD_TFT_DC_GPIO,
            NULL,
            NULL
        );

    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)app_spi_bus_get_host(),
        &io_config,
        &s_io_handle
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create LCD IO: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    ESP_LOGI(TAG, "Creating ILI9341 panel");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_TFT_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_ili9341(
        s_io_handle,
        &panel_config,
        &s_panel_handle
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create ILI9341 panel: %s",
            esp_err_to_name(err)
        );

        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;

        return err;
    }

    ESP_LOGI(TAG, "Resetting display");

    err = esp_lcd_panel_reset(s_panel_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display reset failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Initializing display");

    err = esp_lcd_panel_init(s_panel_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(s_panel_handle, true);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable display: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    ESP_LOGI(
        TAG,
        "ILI9341 initialized: %dx%d RGB565",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    );

    return ESP_OK;
}

esp_err_t display_fill(uint16_t color)
{
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Не выделяем framebuffer на весь экран.
     * Рисуем полосами по 40 строк:
     *
     * 240 * 40 * 2 = 19200 bytes
     */
    const int stripe_height = 40;
    const size_t pixel_count = DISPLAY_WIDTH * stripe_height;

    uint16_t *buffer = heap_caps_malloc(
        pixel_count * sizeof(uint16_t),
        MALLOC_CAP_DMA
    );

    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate display DMA buffer");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        buffer[i] = color;
    }

    for (int y = 0; y < DISPLAY_HEIGHT; y += stripe_height) {
        int y_end = y + stripe_height;

        if (y_end > DISPLAY_HEIGHT) {
            y_end = DISPLAY_HEIGHT;
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_panel_handle,
            0,
            y,
            DISPLAY_WIDTH,
            y_end,
            buffer
        );

        if (err != ESP_OK) {
            free(buffer);

            ESP_LOGE(
                TAG,
                "Draw failed at y=%d: %s",
                y,
                esp_err_to_name(err)
            );

            return err;
        }
    }

    free(buffer);

    return ESP_OK;
}

void display_deinit(void)
{
    if (s_panel_handle != NULL) {
        esp_lcd_panel_del(s_panel_handle);
        s_panel_handle = NULL;
    }

    if (s_io_handle != NULL) {
        esp_lcd_panel_io_del(s_io_handle);
        s_io_handle = NULL;
    }

    ESP_LOGI(TAG, "Display deinitialized");
}