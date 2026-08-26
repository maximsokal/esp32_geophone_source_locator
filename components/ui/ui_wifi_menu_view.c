#include "ui_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "display.h"
#include "ui_theme.h"
#include "ui_wifi_state.h"


static const char *s_menu_items[
    UI_WIFI_MENU_ITEM_COUNT
] = {
    "QUICK SETUP",
    "CONNECT PHONE",
    "OPEN SETUP",
    "HOTSPOT",
    "OPEN DEVICE",
    "FORGET WI-FI",
};


static const int s_menu_y[
    UI_WIFI_MENU_ITEM_COUNT
] = {
    64,
    88,
    112,
    136,
    160,
    184,
};


static esp_err_t ui_wifi_menu_draw_item(
    size_t item,
    bool selected
)
{
    if (
        item >=
        UI_WIFI_MENU_ITEM_COUNT
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
            s_menu_y[item] - 3,
            276,
            22,
            background
        )
    );

    return
        display_draw_text(
            34,
            s_menu_y[item],
            s_menu_items[item],
            foreground,
            background,
            2
        );
}


esp_err_t ui_wifi_menu_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "WI-FI"
        )
    );

    const size_t selection =
        ui_wifi_state_selection();

    for (
        size_t item = 0;
        item < UI_WIFI_MENU_ITEM_COUNT;
        ++item
    ) {
        ESP_ERROR_CHECK(
            ui_wifi_menu_draw_item(
                item,
                item == selection
            )
        );
    }

    return
        ui_common_draw_footer(
            "UP/DOWN SELECT   OK OPEN"
        );
}


esp_err_t ui_wifi_quick_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "QUICK SETUP"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            74,
            "USE CURRENT PRESET?",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            112,
            "SSID: " APP_WIFI_QUICK_SSID,
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            144,
            "PASS: " APP_WIFI_QUICK_PASSWORD,
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            184,
            "OK USE   RIGHT CHANGE",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "LEFT BACK"
        );
}
