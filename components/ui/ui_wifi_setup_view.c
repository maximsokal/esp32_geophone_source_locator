#include "ui_internal.h"

#include "display.h"
#include "ui_theme.h"
#include "wifi_manager.h"
#include "wifi_qr.h"


esp_err_t ui_wifi_connect_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "CONNECT PHONE"
        )
    );

    ESP_ERROR_CHECK(
        wifi_qr_draw(
            104,
            62,
            112
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            48,
            184,
            "SCAN QR AND JOIN WI-FI",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            48,
            198,
            "WAITING FOR PHONE...",
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


esp_err_t ui_wifi_open_setup_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "OPEN SETUP"
        )
    );

    ESP_ERROR_CHECK(
        wifi_qr_draw_url(
            "http://192.168.4.1/setup",
            104,
            62,
            112
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            56,
            184,
            "PHONE CONNECTED",
            UI_COLOR_SUCCESS,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            56,
            198,
            "SCAN QR TO OPEN FORM",
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


esp_err_t ui_wifi_credentials_view_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "HOTSPOT DATA"
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            84,
            "SETUP PAGE OPEN",
            UI_COLOR_SUCCESS,
            UI_COLOR_BACKGROUND,
            2
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            126,
            "ENTER HOTSPOT NAME",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            146,
            "AND PASSWORD ON PHONE",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            34,
            184,
            "WAITING FOR SAVE...",
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
