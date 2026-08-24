#include "display.h"

#include "display_internal.h"


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

    if (
        scale == 0 ||
        scale >
            DISPLAY_FONT_MAX_SCALE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result =
        display_transfer_lock();

    if (result != ESP_OK) {
        return result;
    }

    int start_x =
        x;

    int cell_width =
        DISPLAY_FONT_CELL_WIDTH *
        scale;

    int cell_height =
        DISPLAY_FONT_CELL_HEIGHT *
        scale;

    while (
        *text != '\0'
    ) {
        if (*text == '\n') {
            x =
                start_x;

            y +=
                cell_height;

            text++;

            continue;
        }

        if (
            x + cell_width >
            DISPLAY_WIDTH
        ) {
            x =
                start_x;

            y +=
                cell_height;
        }

        if (
            y + cell_height >
            DISPLAY_HEIGHT
        ) {
            break;
        }

        result =
            display_char_draw_locked(
                x,
                y,
                *text,
                foreground,
                background,
                scale
            );

        if (result != ESP_OK) {
            break;
        }

        x +=
            cell_width;

        text++;
    }

    display_transfer_unlock();

    return result;
}
