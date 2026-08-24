#include "display.h"

#include <stddef.h>
#include <string.h>

#include "display_internal.h"

#include "esp_log.h"


static const char *TAG =
    "DISPLAY_BITMAP";


esp_err_t display_bitmap_draw_locked(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
)
{
    if (
        !display_internal_is_ready()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        width <= 0 ||
        height <= 0 ||
        x < 0 ||
        y < 0 ||
        x + width >
            DISPLAY_WIDTH ||
        y + height >
            DISPLAY_HEIGHT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *buffer =
        display_buffers_dma();

    size_t capacity =
        display_buffers_dma_capacity_pixels();

    int max_rows =
        (int)(
            capacity /
            (size_t)width
        );

    if (
        max_rows >
        DISPLAY_TRANSFER_ROWS
    ) {
        max_rows =
            DISPLAY_TRANSFER_ROWS;
    }

    int source_row =
        0;

    while (
        source_row <
        height
    ) {
        int chunk_height =
            height -
            source_row;

        if (
            chunk_height >
            max_rows
        ) {
            chunk_height =
                max_rows;
        }

        size_t pixel_count =
            (size_t)width *
            chunk_height;

        memcpy(
            buffer,
            pixels +
                (
                    (size_t)source_row *
                    width
                ),
            pixel_count *
                sizeof(uint16_t)
        );

        esp_err_t result =
            display_dma_draw_bitmap_locked(
                x,
                y + source_row,
                x + width,
                y + source_row +
                    chunk_height,
                buffer
            );

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Bitmap draw failed: %s",
                esp_err_to_name(result)
            );

            return result;
        }

        source_row +=
            chunk_height;
    }

    return ESP_OK;
}


esp_err_t display_draw_rgb565_bitmap(
    int x,
    int y,
    int width,
    int height,
    const uint16_t *pixels
)
{
    esp_err_t result =
        display_transfer_lock();

    if (result != ESP_OK) {
        return result;
    }

    result =
        display_bitmap_draw_locked(
            x,
            y,
            width,
            height,
            pixels
        );

    display_transfer_unlock();

    return result;
}
