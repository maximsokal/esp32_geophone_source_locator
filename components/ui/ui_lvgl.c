#include "ui_internal.h"

#include "app_task_config.h"
#include "display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"


static const char *TAG =
    "UI_LVGL";


#define UI_LVGL_BUFFER_ROWS  20


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
            true,

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

    ESP_LOGI(
        TAG,
        "LVGL 9 display ready: %dx%d landscape, Core %d, RGB565 double-buffer",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        APP_TASK_CORE_SERVICES
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
