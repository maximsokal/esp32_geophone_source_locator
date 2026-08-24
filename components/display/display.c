#include "display.h"

#include "display_internal.h"

#include "esp_log.h"


static const char *TAG =
    "DISPLAY";


esp_err_t display_init(void)
{
    if (
        display_driver_is_initialized()
    ) {
        ESP_LOGW(
            TAG,
            "Display already initialized"
        );

        return ESP_OK;
    }

    esp_err_t err =
        display_transfer_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        display_buffers_init();

    if (err != ESP_OK) {
        display_deinit();

        return err;
    }

    err =
        display_dma_init();

    if (err != ESP_OK) {
        display_deinit();

        return err;
    }

    err =
        display_driver_init();

    if (err != ESP_OK) {
        display_deinit();

        return err;
    }

    return ESP_OK;
}


void display_deinit(void)
{
    bool locked =
        display_transfer_lock() ==
        ESP_OK;

    display_driver_deinit();
    display_dma_deinit();
    display_buffers_deinit();

    if (locked) {
        display_transfer_unlock();
    }

    display_transfer_deinit();

    ESP_LOGI(
        TAG,
        "Display deinitialized"
    );
}
