#include "display.h"

#include <stddef.h>

#include "display_internal.h"


esp_err_t display_char_draw_locked(
    int x,
    int y,
    char character,
    uint16_t foreground,
    uint16_t background,
    uint8_t scale
)
{
    if (
        !display_internal_is_ready()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        scale == 0 ||
        scale >
            DISPLAY_FONT_MAX_SCALE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int cell_width =
        DISPLAY_FONT_CELL_WIDTH *
        scale;

    int cell_height =
        DISPLAY_FONT_CELL_HEIGHT *
        scale;

    if (
        x < 0 ||
        y < 0 ||
        x + cell_width >
            DISPLAY_WIDTH ||
        y + cell_height >
            DISPLAY_HEIGHT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *buffer =
        display_buffers_dma();

    size_t required_pixels =
        (size_t)cell_width *
        cell_height;

    if (
        display_buffers_dma_capacity_pixels() <
        required_pixels
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    display_font_render_character(
        buffer,
        cell_width,
        0,
        character,
        foreground,
        background,
        scale
    );

    return
        display_dma_draw_bitmap_locked(
            x,
            y,
            x + cell_width,
            y + cell_height,
            buffer
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
    esp_err_t result =
        display_transfer_lock();

    if (result != ESP_OK) {
        return result;
    }

    result =
        display_char_draw_locked(
            x,
            y,
            character,
            foreground,
            background,
            scale
        );

    display_transfer_unlock();

    return result;
}
