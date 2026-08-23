#include "ui_internal.h"

#include <stdbool.h>

#include "display.h"
#include "ui_theme.h"


static const char *MENU_ITEMS[
    UI_MENU_ITEM_COUNT
] = {
    "MEASUREMENTS",
    "STORAGE",
    "WI-FI",
    "SYSTEM"
};


static const int MENU_Y[
    UI_MENU_ITEM_COUNT
] = {
    70,
    102,
    134,
    166
};


esp_err_t ui_home_draw(
    size_t selected_item
)
{
    if (selected_item >= UI_MENU_ITEM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }


    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "MAIN MENU"
        )
    );


    for (size_t i = 0;
         i < UI_MENU_ITEM_COUNT;
         i++) {

        bool selected =
            i == selected_item;


        uint16_t foreground =
            selected
                ? DISPLAY_COLOR_BLACK
                : UI_COLOR_TEXT;


        uint16_t background =
            selected
                ? UI_COLOR_ACCENT
                : UI_COLOR_BACKGROUND;


        if (selected) {

            ESP_ERROR_CHECK(
                display_fill_rect(
                    22,
                    MENU_Y[i] - 3,
                    276,
                    22,
                    background
                )
            );
        }


        ESP_ERROR_CHECK(
            display_draw_text(
                34,
                MENU_Y[i],
                MENU_ITEMS[i],
                foreground,
                background,
                2
            )
        );
    }


    return ui_common_draw_footer(
        "UP/DOWN SELECT   OK OPEN"
    );
}