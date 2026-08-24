#include "display_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG =
    "DISPLAY_DMA";


static SemaphoreHandle_t s_transfer_done =
    NULL;


static bool display_color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context
)
{
    (void)panel_io;
    (void)event_data;

    SemaphoreHandle_t semaphore =
        (SemaphoreHandle_t)
            user_context;

    if (semaphore == NULL) {
        return false;
    }

    BaseType_t higher_priority_task_woken =
        pdFALSE;

    xSemaphoreGiveFromISR(
        semaphore,
        &higher_priority_task_woken
    );

    return
        higher_priority_task_woken ==
        pdTRUE;
}


bool display_dma_is_ready(void)
{
    return
        s_transfer_done != NULL;
}


esp_err_t display_dma_init(void)
{
    if (
        display_dma_is_ready()
    ) {
        return ESP_OK;
    }

    s_transfer_done =
        xSemaphoreCreateBinary();

    return
        s_transfer_done != NULL
            ? ESP_OK
            : ESP_ERR_NO_MEM;
}


void display_dma_deinit(void)
{
    if (s_transfer_done != NULL) {
        vSemaphoreDelete(
            s_transfer_done
        );

        s_transfer_done =
            NULL;
    }
}


esp_err_t display_dma_register_io(
    esp_lcd_panel_io_handle_t io_handle
)
{
    if (
        io_handle == NULL ||
        s_transfer_done == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done =
            display_color_transfer_done,
    };

    return
        esp_lcd_panel_io_register_event_callbacks(
            io_handle,
            &callbacks,
            s_transfer_done
        );
}


esp_err_t display_dma_draw_bitmap_locked(
    int x_start,
    int y_start,
    int x_end,
    int y_end,
    const uint16_t *pixels
)
{
    esp_lcd_panel_handle_t panel =
        display_transfer_panel();

    if (
        panel == NULL ||
        s_transfer_done == NULL ||
        pixels == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    while (
        xSemaphoreTake(
            s_transfer_done,
            0
        ) == pdTRUE
    ) {
    }

    esp_err_t err =
        esp_lcd_panel_draw_bitmap(
            panel,
            x_start,
            y_start,
            x_end,
            y_end,
            pixels
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        xSemaphoreTake(
            s_transfer_done,
            pdMS_TO_TICKS(1000)
        ) != pdTRUE
    ) {
        ESP_LOGE(
            TAG,
            "LCD transfer timeout"
        );

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
