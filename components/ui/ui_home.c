#include "ui_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display.h"
#include "ui_theme.h"


static const char *MENU_ITEMS[
    UI_MENU_ITEM_COUNT
] = {
    "OBJECTS",
    "MEASUREMENTS",
    "STORAGE",
    "WI-FI",
    "SYSTEM"
};


static const int MENU_Y[
    UI_MENU_ITEM_COUNT
] = {
    66,
    94,
    122,
    150,
    178
};


static esp_err_t ui_home_draw_item(
    size_t item,
    bool selected
)
{
    if (
        item >=
        UI_MENU_ITEM_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    const uint16_t foreground =
        selected
            ? DISPLAY_COLOR_BLACK
            : UI_COLOR_TEXT;


    const uint16_t background =
        selected
            ? UI_COLOR_ACCENT
            : UI_COLOR_BACKGROUND;


    ESP_ERROR_CHECK(
        display_fill_rect(
            22,
            MENU_Y[item] - 3,
            276,
            22,
            background
        )
    );


    return display_draw_text(
        34,
        MENU_Y[item],
        MENU_ITEMS[item],
        foreground,
        background,
        2
    );
}


esp_err_t ui_home_draw(
    size_t selected_item
)
{
    if (
        selected_item >=
        UI_MENU_ITEM_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "MAIN MENU"
        )
    );


    for (
        size_t i = 0;
        i < UI_MENU_ITEM_COUNT;
        ++i
    ) {
        ESP_ERROR_CHECK(
            ui_home_draw_item(
                i,
                i == selected_item
            )
        );
    }


    return ui_common_draw_footer(
        "UP/DOWN SELECT   OK OPEN"
    );
}


esp_err_t ui_home_update_selection(
    size_t previous_item,
    size_t selected_item
)
{
    if (
        previous_item >=
            UI_MENU_ITEM_COUNT ||
        selected_item >=
            UI_MENU_ITEM_COUNT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    if (
        previous_item ==
        selected_item
    ) {
        return ESP_OK;
    }


    ESP_ERROR_CHECK(
        ui_home_draw_item(
            previous_item,
            false
        )
    );


    return ui_home_draw_item(
        selected_item,
        true
    );
}