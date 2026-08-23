#include "ui_internal.h"

#include "app_config.h"
#include "display.h"
#include "ui_theme.h"
#include "wifi_qr.h"

#include "esp_log.h"


static const char *TAG =
    "UI_WIFI";


esp_err_t ui_wifi_draw(void)
{
    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "WI-FI"
        )
    );


    /*
     * Draw the real Wi-Fi connection QR code.
     *
     * The QR bitmap is generated completely in memory
     * and then transferred to the TFT in large blocks.
     */
    esp_err_t qr_err =
        wifi_qr_draw(
            104,
            61,
            112
        );


    if (qr_err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to draw Wi-Fi QR: %s",
            esp_err_to_name(qr_err)
        );


        ESP_ERROR_CHECK(
            display_draw_text(
                116,
                108,
                "QR ERROR",
                UI_COLOR_WARNING,
                UI_COLOR_BACKGROUND,
                1
            )
        );
    }


    ESP_ERROR_CHECK(
        display_draw_text(
            62,
            178,
            "SSID: " APP_WIFI_AP_SSID,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            62,
            190,
            "PASS: " APP_WIFI_AP_PASSWORD,
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );


    ESP_ERROR_CHECK(
        display_draw_text(
            62,
            202,
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