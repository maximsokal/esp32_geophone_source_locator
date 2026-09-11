#include "ui_internal.h"

#include <stdint.h>

#include "app_task_config.h"
#include "display.h"
#include "spi_bus.h"

#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


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

#define UI_FLUSH_PERF_PERIOD_US  10000000ULL


typedef struct
{
    lv_display_t *display;
    SemaphoreHandle_t flush_done;

} ui_lvgl_flush_sync_t;


static StaticSemaphore_t s_flush_done_storage;
static ui_lvgl_flush_sync_t s_flush_sync;
static uint64_t s_flush_perf_started_us;
static uint64_t s_flush_wait_count;
static uint64_t s_flush_wait_total_us;
static uint64_t s_flush_wait_max_us;


static bool ui_lvgl_flush_io_ready_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context
);

static void ui_lvgl_flush_wait_callback(
    lv_display_t *display
);

static esp_err_t ui_lvgl_install_flush_wait_locked(
    lv_display_t *display
);

static void ui_lvgl_prepare_flush_deinit_locked(
    lv_display_t *display
);


static void ui_lvgl_flush_perf_reset(
    uint64_t now_us
)
{
    s_flush_perf_started_us = now_us;
    s_flush_wait_count = 0U;
    s_flush_wait_total_us = 0U;
    s_flush_wait_max_us = 0U;
}


static bool ui_lvgl_flush_io_ready_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context
)
{
    (void)panel_io;
    (void)event_data;

    ui_lvgl_flush_sync_t *sync =
        (ui_lvgl_flush_sync_t *)user_context;

    if (sync == NULL) {
        return false;
    }

    BaseType_t higher_priority_task_woken = pdFALSE;

    /*
     * Do not clear LVGL's flushing flag from the ISR. If DMA completes before
     * LVGL reaches wait_for_flushing(), leaving the flag set forces LVGL to
     * consume this exact semaphore token instead of carrying it into the
     * next flush.
     */
    if (sync->flush_done != NULL) {
        (void)xSemaphoreGiveFromISR(
            sync->flush_done,
            &higher_priority_task_woken
        );
    }

    return higher_priority_task_woken == pdTRUE;
}


static void ui_lvgl_flush_wait_callback(
    lv_display_t *display
)
{
    /* LVGL calls this only after the async flush callback was installed. */
    configASSERT(s_flush_sync.flush_done != NULL);

    const uint64_t started_us =
        (uint64_t)esp_timer_get_time();

    const BaseType_t taken =
        xSemaphoreTake(
            s_flush_sync.flush_done,
            portMAX_DELAY
        );

    if (taken != pdTRUE) {
        ESP_LOGE(
            TAG,
            "TFT flush completion semaphore failed"
        );
        return;
    }

    lvgl_port_flush_ready(display);

    const uint64_t now_us =
        (uint64_t)esp_timer_get_time();
    const uint64_t wait_us =
        now_us >= started_us
            ? now_us - started_us
            : 0U;

    ++s_flush_wait_count;
    s_flush_wait_total_us += wait_us;
    if (wait_us > s_flush_wait_max_us) {
        s_flush_wait_max_us = wait_us;
    }

    if (
        s_flush_perf_started_us == 0U ||
        now_us - s_flush_perf_started_us >= UI_FLUSH_PERF_PERIOD_US
    ) {
        ESP_LOGI(
            TAG,
            "UI_FLUSH_PERF: waits=%llu total_wait_us=%llu max_wait_us=%llu avg_wait_us=%llu",
            (unsigned long long)s_flush_wait_count,
            (unsigned long long)s_flush_wait_total_us,
            (unsigned long long)s_flush_wait_max_us,
            (unsigned long long)(
                s_flush_wait_count > 0U
                    ? s_flush_wait_total_us / s_flush_wait_count
                    : 0U
            )
        );
        ui_lvgl_flush_perf_reset(now_us);
    }
}


static esp_err_t ui_lvgl_install_flush_wait_locked(
    lv_display_t *display
)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_flush_sync.flush_done == NULL) {
        s_flush_sync.flush_done =
            xSemaphoreCreateBinaryStatic(
                &s_flush_done_storage
            );
    }

    if (s_flush_sync.flush_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* A previous lifecycle may have drained a transfer after its waiter left. */
    while (
        xSemaphoreTake(
            s_flush_sync.flush_done,
            0
        ) == pdTRUE
    ) {
    }

    s_flush_sync.display = display;
    ui_lvgl_flush_perf_reset(
        (uint64_t)esp_timer_get_time()
    );

    lv_display_set_flush_wait_cb(
        display,
        ui_lvgl_flush_wait_callback
    );

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done =
            ui_lvgl_flush_io_ready_callback,
    };

    esp_lcd_panel_io_handle_t io =
        display_io_handle();
    if (io == NULL) {
        lv_display_set_flush_wait_cb(display, NULL);
        s_flush_sync.display = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err =
        esp_lcd_panel_io_register_event_callbacks(
            io,
            &callbacks,
            &s_flush_sync
        );

    if (err != ESP_OK) {
        lv_display_set_flush_wait_cb(display, NULL);
        s_flush_sync.display = NULL;
        return err;
    }

    return ESP_OK;
}


static void ui_lvgl_prepare_flush_deinit_locked(
    lv_display_t *display
)
{
    if (display == NULL) {
        return;
    }

    /* Drain all queued LCD DMA while the display pointer is still valid. */
    esp_lcd_panel_io_handle_t io =
        display_io_handle();
    if (io != NULL) {
        const esp_err_t err =
            esp_lcd_panel_io_tx_param(
                io,
                -1,
                NULL,
                0
            );
        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to drain TFT DMA before LVGL teardown: %s",
                esp_err_to_name(err)
            );
        }
    }

    lv_display_set_flush_wait_cb(display, NULL);
    s_flush_sync.display = NULL;

    if (s_flush_sync.flush_done != NULL) {
        while (
            xSemaphoreTake(
                s_flush_sync.flush_done,
                0
            ) == pdTRUE
        ) {
        }
    }
}


static esp_err_t ui_lvgl_quiesce_shared_spi(void *context)
{
    (void)context;

    /*
     * Stop the LVGL task from submitting another asynchronous color flush.
     * A zero timeout in esp_lvgl_port means wait indefinitely.
     */
    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_lcd_panel_io_handle_t io =
        display_io_handle();

    if (io == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * esp_lcd_panel_io_tx_param() first acquires the LCD SPI device and drains
     * every queued tx_color transaction. With lcd_cmd=-1 and no parameters it
     * becomes a pure synchronization barrier: no LCD command is emitted.
     *
     * This is required before SDSPI starts its polling transaction on SPI2.
     */
    esp_err_t err =
        esp_lcd_panel_io_tx_param(
            io,
            -1,
            NULL,
            0
        );

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

    if (!lvgl_port_lock(1000)) {
        ui_lvgl_deinit();
        return ESP_ERR_TIMEOUT;
    }

    err =
        ui_lvgl_install_flush_wait_locked(
            context->display
        );

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to install blocking TFT flush wait: %s",
            esp_err_to_name(err)
        );
        ui_lvgl_deinit();
        return err;
    }

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
            ui_lvgl_prepare_flush_deinit_locked(
                context->display
            );
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
