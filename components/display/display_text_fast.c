#include "display.h"

#include <stddef.h>

#include "display_internal.h"


esp_err_t display_draw_text_fast(
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

    if (
        scale == 0 ||
        scale >
            DISPLAY_FONT_MAX_SCALE ||
        x < 0 ||
        y < 0 ||
        x >= DISPLAY_WIDTH ||
        y >= DISPLAY_HEIGHT
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

    int cell_width =
        DISPLAY_FONT_CELL_WIDTH *
        scale;

    int cell_height =
        DISPLAY_FONT_CELL_HEIGHT *
        scale;

    size_t max_characters =
        (size_t)(
            (
                DISPLAY_WIDTH -
                x
            ) /
            cell_width
        );

    if (max_characters == 0) {
        result =
            ESP_ERR_INVALID_SIZE;

        goto done;
    }

    uint16_t *pixels =
        display_buffers_text();

    size_t capacity =
        display_buffers_text_capacity_pixels();

    const char *cursor =
        text;

    int current_y =
        y;

    while (
        *cursor != '\0' &&
        current_y +
            cell_height <=
            DISPLAY_HEIGHT
    ) {
        if (*cursor == '\n') {
            current_y +=
                cell_height;

            cursor++;

            continue;
        }

        size_t run_length =
            0;

        while (
            cursor[run_length] != '\0' &&
            cursor[run_length] != '\n' &&
            run_length <
                max_characters
        ) {
            run_length++;
        }

        if (run_length == 0) {
            break;
        }

        int run_width =
            (int)run_length *
            cell_width;

        size_t required_pixels =
            (size_t)run_width *
            cell_height;

        if (
            required_pixels >
            capacity
        ) {
            result =
                ESP_ERR_INVALID_SIZE;

            break;
        }

        for (
            size_t i = 0;
            i < run_length;
            ++i
        ) {
            display_font_render_character(
                pixels,
                run_width,
                (int)i *
                    cell_width,
                cursor[i],
                foreground,
                background,
                scale
            );
        }

        result =
            display_bitmap_draw_locked(
                x,
                current_y,
                run_width,
                cell_height,
                pixels
            );

        if (result != ESP_OK) {
            break;
        }

        cursor +=
            run_length;

        if (*cursor == '\n') {
            cursor++;

            current_y +=
                cell_height;

            continue;
        }

        if (*cursor != '\0') {
            current_y +=
                cell_height;
        }
    }

done:
    display_transfer_unlock();

    return result;
}
