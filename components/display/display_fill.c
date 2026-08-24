#include "display.h"

#include <stddef.h>

#include "display_internal.h"

#include "esp_log.h"


static const char *TAG =
    "DISPLAY_FILL";


esp_err_t display_fill_rect(
    int x,
    int y,
    int width,
    int height,
    uint16_t color
)
{
    if (
        width <= 0 ||
        height <= 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result =
        display_transfer_lock();

    if (result != ESP_OK) {
        return result;
    }

    if (
        !display_internal_is_ready()
    ) {
        result =
            ESP_ERR_INVALID_STATE;

        goto done;
    }

    if (x < 0) {
        width +=
            x;

        x =
            0;
    }

    if (y < 0) {
        height +=
            y;

        y =
            0;
    }

    if (
        x >= DISPLAY_WIDTH ||
        y >= DISPLAY_HEIGHT ||
        width <= 0 ||
        height <= 0
    ) {
        result =
            ESP_OK;

        goto done;
    }

    if (
        x + width >
        DISPLAY_WIDTH
    ) {
        width =
            DISPLAY_WIDTH - x;
    }

    if (
        y + height >
        DISPLAY_HEIGHT
    ) {
        height =
            DISPLAY_HEIGHT - y;
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

    int remaining_height =
        height;

    int current_y =
        y;

    while (
        remaining_height > 0
    ) {
        int chunk_height =
            remaining_height;

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

        for (
            size_t i = 0;
            i < pixel_count;
            ++i
        ) {
            buffer[i] =
                color;
        }

        result =
            display_dma_draw_bitmap_locked(
                x,
                current_y,
                x + width,
                current_y +
                    chunk_height,
                buffer
            );

        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Rectangle draw failed: %s",
                esp_err_to_name(result)
            );

            goto done;
        }

        current_y +=
            chunk_height;

        remaining_height -=
            chunk_height;
    }

done:
    display_transfer_unlock();

    return result;
}


esp_err_t display_fill(
    uint16_t color
)
{
    return
        display_fill_rect(
            0,
            0,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            color
        );
}
