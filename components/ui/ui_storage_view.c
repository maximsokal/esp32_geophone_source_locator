#include "ui_storage_view.h"

#include "display.h"
#include "ui_internal.h"
#include "ui_theme.h"


static const char *ACTION_NAMES[
    UI_STORAGE_ACTION_COUNT
] = {
    "CARD INFO",
    "FREE SPACE",
    "ERASE DATA"
};


static const int ACTION_Y[
    UI_STORAGE_ACTION_COUNT
] = {
    88,
    122,
    156
};


static esp_err_t ui_storage_view_draw_action(
    size_t action,
    bool selected
)
{
    if (
        action >=
        UI_STORAGE_ACTION_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t foreground =
        selected
        ? DISPLAY_COLOR_BLACK
        : UI_COLOR_TEXT;

    uint16_t background =
        selected
        ? UI_COLOR_ACCENT
        : UI_COLOR_BACKGROUND;

    ESP_ERROR_CHECK(
        display_fill_rect(
            22,
            ACTION_Y[action] - 4,
            276,
            26,
            background
        )
    );

    return
        display_draw_text_fast(
            34,
            ACTION_Y[action],
            ACTION_NAMES[action],
            foreground,
            background,
            2
        );
}


esp_err_t ui_storage_view_draw_menu(
    size_t selected_action,
    const char *notice,
    bool warning
)
{
    if (
        selected_action >=
        UI_STORAGE_ACTION_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "STORAGE"
        )
    );

    for (
        size_t i = 0;
        i <
            UI_STORAGE_ACTION_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            ui_storage_view_draw_action(
                i,
                i ==
                    selected_action
            )
        );
    }

    if (
        notice != NULL &&
        notice[0] != '\0'
    ) {
        ESP_ERROR_CHECK(
            display_draw_text_fast(
                14,
                194,
                notice,
                warning
                    ? UI_COLOR_WARNING
                    : UI_COLOR_SUCCESS,
                UI_COLOR_BACKGROUND,
                1
            )
        );
    }

    return
        ui_common_draw_footer(
            "UP/DOWN  OK OPEN  LEFT BACK"
        );
}


esp_err_t ui_storage_view_update_selection(
    size_t previous_action,
    size_t selected_action
)
{
    if (
        previous_action >=
            UI_STORAGE_ACTION_COUNT ||
        selected_action >=
            UI_STORAGE_ACTION_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(
        ui_storage_view_draw_action(
            previous_action,
            false
        )
    );

    return
        ui_storage_view_draw_action(
            selected_action,
            true
        );
}
