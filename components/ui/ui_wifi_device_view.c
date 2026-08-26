#include "ui_internal.h"

#include <stdio.h>

#include "display.h"
#include "ui_theme.h"
#include "wifi_manager.h"
#include "wifi_qr.h"


esp_err_t ui_wifi_device_view_draw(void)
{
    char ip[16];

    ESP_ERROR_CHECK(
        ui_common_begin_screen(
            "OPEN DEVICE"
        )
    );

    if (
        wifi_manager_sta_ip_string(
            ip,
            sizeof(ip)
        ) != ESP_OK
    ) {
        ESP_ERROR_CHECK(
            display_draw_text(
                60,
                100,
                "DEVICE NOT CONNECTED",
                UI_COLOR_WARNING,
                UI_COLOR_BACKGROUND,
                1
            )
        );

        ESP_ERROR_CHECK(
            display_draw_text(
                60,
                130,
                "TURN ON SAVED HOTSPOT",
                UI_COLOR_TEXT,
                UI_COLOR_BACKGROUND,
                1
            )
        );

        return
            ui_common_draw_footer(
                "LEFT MENU"
            );
    }

    char url[32];

    snprintf(
        url,
        sizeof(url),
        "http://%s/",
        ip
    );

    ESP_ERROR_CHECK(
        wifi_qr_draw_url(
            url,
            104,
            55,
            112
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            76,
            176,
            "SCAN TO OPEN DEVICE",
            UI_COLOR_TEXT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    ESP_ERROR_CHECK(
        display_draw_text(
            94,
            194,
            ip,
            UI_COLOR_ACCENT,
            UI_COLOR_BACKGROUND,
            1
        )
    );

    return
        ui_common_draw_footer(
            "LEFT MENU"
        );
}
