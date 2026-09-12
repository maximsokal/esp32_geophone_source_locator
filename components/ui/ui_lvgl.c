#include "ui_internal.h"

#include <stdint.h>

#include "app_task_config.h"
#include "display.h"
#include "spi_bus.h"

#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"


static const char *TAG =
    "UI_LVGL";


/*
 * Keep one TFT draw buffer in DMA-capable internal SRAM, but do not make it
 * smaller than the rendering path can use efficiently. A 4-row buffer made a
 * full 320x240 invalidation 60 separate SPI flushes; 8 rows halves that
 * transaction count while costing only 320 * 8 * 2 = 5120 bytes. The buffer
 * remains single and internal so SDSPI/Wi-Fi DMA headroom is still explicit.
 */
#define UI_LVGL_BUFFER_ROWS  8


static esp_err_t ui_lvgl_drain_panel_io_locked(void)
{
    esp_lcd_panel_io_handle_t io =
        display_io_handle();

    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * esp_lcd_panel_io_tx_param() is a synchronous polling transaction. The
     * SPI LCD implementation first waits for every queued tx_color operation
     * to complete. lcd_cmd=-1, param=NULL, size=0 emits no command, so this is
     * a pure queue-drain barrier.
     *
     * Do not install another on_color_trans_done callback here. esp_lvgl_port
     * owns that callback after lvgl_port_add_disp(); overwriting it breaks the
     * port's flush-completion contract and produced the runtime warning:
     * "Callback on_color_trans_done was already set and now it was overwritten".
     */
    return esp_lcd_panel_io_tx_param(
        io,
        -1,
        NULL,
        0
    );
}


static esp_err_t ui_lvgl_quiesce_shared_spi(void *context)
{
    (void)context;

    /*
     * Stop the LVGL task from submitting another asynchronous color flush.
     * A zero timeout in esp_lvgl_port means wait indefinitely. Once locked,
     * drain the LCD IO queue before SDSPI starts using the shared SPI2 bus.
     */
    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err =
        ui_lvgl_drain_panel_io_locked();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to drain LCD SPI queue before SD access: %s",
            esp_err_to_name(err)
        );

        lvgl_port_unlock();
        return err;
    }

    return ESP_OK;
}


static void ui_lvgl_resume_shared_spi(void *context)
{
    (void)context;
    lvgl_port_unlock();
}


esp_err_t ui_lvgl_init(void)
{
    ui_context_t *context =
        ui_context();

    if (context->port_ready) {
        return ESP_OK;
    }

    if (!display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port_config =
        ESP_LVGL_PORT_INIT_CONFIG();

    port_config.task_priority =
        APP_TASK_PRIORITY_LVGL;

    port_config.task_stack =
        APP_TASK_STACK_LVGL;

    port_config.task_affinity =
        APP_TASK_CORE_SERVICES;

    port_config.task_max_sleep_ms =
        250;

    esp_err_t err =
        lvgl_port_init(
            &port_config
        );

    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_display_cfg_t display_config = {
        .io_handle =
            display_io_handle(),

        .panel_handle =
            display_panel_handle(),

        .buffer_size =
            DISPLAY_WIDTH *
            UI_LVGL_BUFFER_ROWS,

        .double_buffer =
            false,

        .trans_size =
            0,

        .hres =
            DISPLAY_WIDTH,

        .vres =
            DISPLAY_HEIGHT,

        .monochrome =
            false,

        .color_format =
            LV_COLOR_FORMAT_RGB565,

        /*
         * The physical ILI9341 is native 240x320. esp_lvgl_port
         * owns the one and only rotation decision and applies the
         * verified 320x240 landscape orientation here.
         */
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },

        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    context->display =
        lvgl_port_add_disp(
            &display_config
        );

    if (context->display == NULL) {
        (void)lvgl_port_deinit();

        return ESP_FAIL;
    }

    context->port_ready =
        true;

    err =
        app_spi_bus_register_display_guard(
            ui_lvgl_quiesce_shared_spi,
            ui_lvgl_resume_shared_spi,
            NULL
        );

    if (err != ESP_OK) {
        ui_lvgl_deinit();
        return err;
    }

    ESP_LOGI(
        TAG,
        "LVGL 9 display ready: %dx%d landscape, Core %d, RGB565 %d-row single DMA buffer",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        APP_TASK_CORE_SERVICES,
        UI_LVGL_BUFFER_ROWS
    );

    ESP_LOGI(
        TAG,
        "DMA_RUNTIME owner=tft location=internal bytes=%u mode=lvgl_dma_buffer",
        (unsigned)(DISPLAY_WIDTH * UI_LVGL_BUFFER_ROWS * sizeof(uint16_t))
    );

    return ESP_OK;
}


void ui_lvgl_deinit(void)
{
    ui_context_t *context =
        ui_context();

    if (!context->port_ready) {
        return;
    }

    /*
     * Wait for an active SD operation to leave the shared-bus guard before
     * tearing down LVGL. After this point no new storage operation will try
     * to lock a deinitialized LVGL mutex.
     */
    app_spi_bus_unregister_display_guard();

    if (context->display != NULL) {
        if (!lvgl_port_lock(0)) {
            ESP_LOGW(
                TAG,
                "Unable to lock LVGL while draining TFT DMA"
            );
        }
        else {
            const esp_err_t drain_err =
                ui_lvgl_drain_panel_io_locked();
            if (drain_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Failed to drain TFT DMA before LVGL teardown: %s",
                    esp_err_to_name(drain_err)
                );
            }
            lvgl_port_unlock();
        }
    }

    if (context->display != NULL) {
        (void)lvgl_port_remove_disp(
            context->display
        );

        context->display =
            NULL;
    }

    (void)lvgl_port_deinit();

    context->port_ready =
        false;

    context->root =
        NULL;

    ESP_LOGI(
        TAG,
        "LVGL deinitialized"
    );
}
