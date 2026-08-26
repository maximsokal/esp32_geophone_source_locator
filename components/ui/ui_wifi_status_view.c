#include "ui_internal.h"

#include <stdio.h>

#include "display.h"
#include "ui_theme.h"
#include "wifi_manager.h"


esp_err_t ui_wifi_hotspot_view_draw(void)
{
    char ssid[33] =
        "<NOT SAVED>";

    (void)
        wifi_manager_get_saved_ssid(
            ssid,
            sizeof(ssid)
        );

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "PHONE HOTSPOT"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            76,
            "HOTSPOT SAVED",
            UI_COLOR_SUCCESS,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    char line[48];

    snprintf(
        line,
        sizeof(line),
        "SSID: %s",
        ssid[0] != '\0'
            ? ssid
            : "<NOT SAVED>"
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            118,
            line,
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            148,
            "TURN ON PHONE HOTSPOT",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            184,
            "WAITING FOR ESP IP...",
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "AUTO NEXT   LEFT MENU"
        );
}


esp_err_t ui_wifi_forget_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "FORGET WI-FI"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            92,
            "DELETE SAVED HOTSPOT?",
            UI_COLOR_WARNING,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            132,
            "OK = DELETE",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    return
        ui_common_draw_footer(
            "LEFT CANCEL"
        );
}
