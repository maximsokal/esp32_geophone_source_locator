#include "ui_internal.h"

#include "display.h"
#include "ui_theme.h"


esp_err_t ui_common_draw_box(
    int x,
    int y,
    int width,
    int height,
    uint16_t color
)
{
    if (width < 2 || height < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        display_fill_rect(
            x,
            y,
            width,
            1,
            color
        )
    );

    ESP_ERROR_CHECK(
        display_fill_rect(
            x,
            y + height - 1,
            width,
            1,
            color
        )
    );

    ESP_ERROR_CHECK(
        display_fill_rect(
            x,
            y,
            1,
            height,
            color
        )
    );

    ESP_ERROR_CHECK(
        display_fill_rect(
            x + width - 1,
            y,
            1,
            height,
            color
        )
    );

    return ESP_OK;
}


esp_err_t ui_common_begin_screen(
    const char *title
)
{
    if (title == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        display_fill(
            UI_COLOR_BACKGROUND
        )
    );

    ESP_ERROR_CHECK(
        ui_status_bar_draw()
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            10,
            UI_TITLE_Y,
            title,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_fill_rect(
            0,
            58,
            DISPLAY_WIDTH,
            1,
            UI_COLOR_SEPARATOR
        )
    );

    return ESP_OK;
}


esp_err_t ui_common_draw_footer(
    const char *text
)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        display_fill_rect(
            0,
            UI_FOOTER_SEPARATOR_Y,
            DISPLAY_WIDTH,
            1,
            UI_COLOR_SEPARATOR
        )
    );

    return display_draw_text(
        10,
        UI_FOOTER_TEXT_Y,
        text,
        UI_COLOR_SECONDARY,
        UI_COLOR_BACKGROUND,
        1
    );
}