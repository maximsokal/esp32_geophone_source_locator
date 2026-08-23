#include "ui_internal.h"

#include "app_config.h"
#include "display.h"
#include "ui_theme.h"


esp_err_t ui_wifi_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "WI-FI"
        )
    );


    /*
     * Reserved area for the real Wi-Fi QR code.
     *
     * The next implementation slice will replace this
     * placeholder with a generated QR bitmap.
     */
    const int qr_x = 110;
    const int qr_y = 64;
    const int qr_size = 100;


    ESP_ERROR_CHECK(
        ui_common_draw_box(
            qr_x,
            qr_y,
            qr_size,
            qr_size,
            UI_COLOR_SECONDARY
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            qr_x + 38,
            qr_y + 30,
            "QR",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            2
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            qr_x + 20,
            qr_y + 58,
            "NEXT SLICE",
            UI_COLOR_SECONDARY,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            76,
            170,
            "SSID: " APP_WIFI_AP_SSID,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            76,
            183,
            "PASS: " APP_WIFI_AP_PASSWORD,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            76,
            196,
            "URL: " APP_WIFI_AP_ADDRESS,
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    return ui_common_draw_footer(
        "LEFT BACK"
    );
}