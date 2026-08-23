#include "display.h"

#include <stdlib.h>
#include <stdbool.h>

#include "font5x7.h"

#include "board_config.h"
#include "spi_bus.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG = "DISPLAY";

#define DISPLAY_TRANSFER_ROWS       20

#define FONT_GLYPH_WIDTH            5
#define FONT_GLYPH_HEIGHT           7
#define FONT_CELL_WIDTH             6
#define FONT_CELL_HEIGHT            8
#define FONT_MAX_SCALE              4


static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;

static SemaphoreHandle_t s_transfer_done = NULL;

static uint16_t *s_transfer_buffer = NULL;


/*
 * The SPI LCD color transfer is asynchronous.
 *
 * The transfer-done callback allows the drawing functions to wait
 * until DMA has finished using the current pixel buffer.
 */
static bool display_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context
)
{
    (void)panel_io;
    (void)event_data;

    SemaphoreHandle_t semaphore =
        (SemaphoreHandle_t)user_context;

    BaseType_t higher_priority_task_woken = pdFALSE;

    xSemaphoreGiveFromISR(
        semaphore,
        &higher_priority_task_woken
    );

    return higher_priority_task_woken == pdTRUE;
}


static esp_err_t display_draw_bitmap_sync(
    int x_start,
    int y_start,
    int x_end,
    int y_end,
    const uint16_t *pixels
)
{
    if (s_panel_handle == NULL ||
        s_transfer_done == NULL ||
        pixels == NULL) {

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Remove a stale completion signal if one exists.
     */
    while (xSemaphoreTake(s_transfer_done, 0) == pdTRUE) {
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel_handle,
        x_start,
        y_start,
        x_end,
        y_end,
        pixels
    );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * The buffer must remain valid until SPI DMA completes.
     */
    if (xSemaphoreTake(
            s_transfer_done,
            pdMS_TO_TICKS(1000)
        ) != pdTRUE) {

        ESP_LOGE(TAG, "LCD transfer timeout");

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


esp_err_t display_init(void)
{
    if (s_panel_handle != NULL) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    s_transfer_done = xSemaphoreCreateBinary();

    if (s_transfer_done == NULL) {
        ESP_LOGE(TAG, "Failed to create transfer semaphore");
        return ESP_ERR_NO_MEM;
    }

    /*
     * One reusable DMA-capable drawing buffer.
     *
     * 320 * 20 * 2 bytes = 12.8 KiB.
     */
    const size_t transfer_pixel_count =
        DISPLAY_WIDTH * DISPLAY_TRANSFER_ROWS;

    s_transfer_buffer = heap_caps_malloc(
        transfer_pixel_count * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );

    if (s_transfer_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LCD transfer buffer");

        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;

        return ESP_ERR_NO_MEM;
    }

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

        goto fail;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = display_color_transfer_done,
    };

    err = esp_lcd_panel_io_register_event_callbacks(
        s_io_handle,
        &callbacks,
        s_transfer_done
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register LCD callbacks: %s",
            esp_err_to_name(err)
        );

        goto fail;
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

        goto fail;
    }

    ESP_LOGI(TAG, "Resetting display");

    err = esp_lcd_panel_reset(s_panel_handle);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Display reset failed: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

    ESP_LOGI(TAG, "Initializing display");

    err = esp_lcd_panel_init(s_panel_handle);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Display init failed: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

	/*
	 * Rotate the native 240x320 panel into 320x240 landscape.
	 *
	 * No mirroring matches the final physical mounting orientation.
	 */
	err = esp_lcd_panel_swap_xy(
	    s_panel_handle,
	    true
	);
	
	if (err != ESP_OK) {
	    ESP_LOGE(
	        TAG,
	        "Failed to swap display axes: %s",
	        esp_err_to_name(err)
	    );
	
	    goto fail;
	}
	
	err = esp_lcd_panel_mirror(
	    s_panel_handle,
	    false,
	    false
	);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to mirror display: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

    err = esp_lcd_panel_disp_on_off(
        s_panel_handle,
        true
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable display: %s",
            esp_err_to_name(err)
        );

        goto fail;
    }

    ESP_LOGI(
        TAG,
        "ILI9341 initialized: %dx%d landscape RGB565",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    );

    return ESP_OK;


fail:

    display_deinit();

    return err;
}


esp_err_t display_fill_rect(
    int x,
    int y,
    int width,
    int height,
    uint16_t color
)
{
    if (s_panel_handle == NULL ||
        s_transfer_buffer == NULL) {

        return ESP_ERR_INVALID_STATE;
    }

    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Clip the rectangle to the visible display area.
     */
    if (x < 0) {
        width += x;
        x = 0;
    }

    if (y < 0) {
        height += y;
        y = 0;
    }

    if (x >= DISPLAY_WIDTH ||
        y >= DISPLAY_HEIGHT ||
        width <= 0 ||
        height <= 0) {

        return ESP_OK;
    }

    if (x + width > DISPLAY_WIDTH) {
        width = DISPLAY_WIDTH - x;
    }

    if (y + height > DISPLAY_HEIGHT) {
        height = DISPLAY_HEIGHT - y;
    }

    int remaining_height = height;
    int current_y = y;

    while (remaining_height > 0) {
        int chunk_height = remaining_height;

        if (chunk_height > DISPLAY_TRANSFER_ROWS) {
            chunk_height = DISPLAY_TRANSFER_ROWS;
        }

        const size_t pixel_count =
            (size_t)width * chunk_height;

        for (size_t i = 0; i < pixel_count; ++i) {
            s_transfer_buffer[i] = color;
        }

        esp_err_t err = display_draw_bitmap_sync(
            x,
            current_y,
            x + width,
            current_y + chunk_height,
            s_transfer_buffer
        );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Rectangle draw failed: %s",
                esp_err_to_name(err)
            );

            return err;
        }

        current_y += chunk_height;
        remaining_height -= chunk_height;
    }

    return ESP_OK;
}


esp_err_t display_fill(uint16_t color)
{
    return display_fill_rect(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        color
    );
}


esp_err_t display_draw_char(
    int x,
    int y,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    if (s_panel_handle == NULL ||
        s_transfer_buffer == NULL) {

        return ESP_ERR_INVALID_STATE;
    }

    if (scale == 0 || scale > FONT_MAX_SCALE) {
        return ESP_ERR_INVALID_ARG;
    }

    const int cell_width =
        FONT_CELL_WIDTH * scale;

    const int cell_height =
        FONT_CELL_HEIGHT * scale;

    if (x < 0 ||
        y < 0 ||
        x + cell_width > DISPLAY_WIDTH ||
        y + cell_height > DISPLAY_HEIGHT) {

        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *glyph =
        font5x7_get_glyph(character);

    /*
     * Render the complete character cell into the reusable
     * transfer buffer before sending it to the LCD.
     */
    for (int py = 0; py < cell_height; ++py) {
        const int source_y = py / scale;

        for (int px = 0; px < cell_width; ++px) {
            const int source_x = px / scale;

            bool pixel_on = false;

            if (source_x < FONT_GLYPH_WIDTH &&
                source_y < FONT_GLYPH_HEIGHT) {

                pixel_on =
                    (glyph[source_x] & (1U << source_y)) != 0;
            }

            s_transfer_buffer[
                py * cell_width + px
            ] = pixel_on
                ? foreground
                : background;
        }
    }

    return display_draw_bitmap_sync(
        x,
        y,
        x + cell_width,
        y + cell_height,
        s_transfer_buffer
    );
}


esp_err_t display_draw_text(
    int x,
    int y,
    const char *text,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (scale == 0 || scale > FONT_MAX_SCALE) {
        return ESP_ERR_INVALID_ARG;
    }

    const int start_x = x;

    const int cell_width =
        FONT_CELL_WIDTH * scale;

    const int cell_height =
        FONT_CELL_HEIGHT * scale;

    while (*text != '\0') {
        if (*text == '\n') {
            x = start_x;
            y += cell_height;

            text++;
            continue;
        }

        /*
         * Wrap text when the next character does not fit.
         */
        if (x + cell_width > DISPLAY_WIDTH) {
            x = start_x;
            y += cell_height;
        }

        if (y + cell_height > DISPLAY_HEIGHT) {
            break;
        }

        esp_err_t err = display_draw_char(
            x,
            y,
            *text,
            foreground,
            background,
            scale
        );

        if (err != ESP_OK) {
            return err;
        }

        x += cell_width;
        text++;
    }

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

    if (s_transfer_buffer != NULL) {
        free(s_transfer_buffer);
        s_transfer_buffer = NULL;
    }

    if (s_transfer_done != NULL) {
        vSemaphoreDelete(s_transfer_done);
        s_transfer_done = NULL;
    }

    ESP_LOGI(TAG, "Display deinitialized");
}