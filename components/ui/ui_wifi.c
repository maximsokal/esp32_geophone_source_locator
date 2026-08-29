#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "wifi_manager.h"


#define UI_WIFI_COLOR_TEXT     0xF3F7FB
#define UI_WIFI_COLOR_MUTED    0x94A7BC
#define UI_WIFI_COLOR_ACCENT   0x21C7D9
#define UI_WIFI_COLOR_GOOD     0x4CD681
#define UI_WIFI_COLOR_WARNING  0xF3B84B


static lv_obj_t *ui_wifi_label(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    lv_obj_t *label =
        lv_label_create(
            parent
        );

    if (label == NULL) {
        return NULL;
    }

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_width(
        label,
        width
    );

    lv_label_set_long_mode(
        label,
        LV_LABEL_LONG_CLIP
    );

    lv_label_set_text(
        label,
        text != NULL
            ? text
            : ""
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


static void ui_wifi_render_setup_qr(
    lv_obj_t *screen
)
{
#if LV_USE_QRCODE
    char payload[128];

    const int length =
        snprintf(
            payload,
            sizeof(payload),
            "WIFI:T:WPA;S:%s;P:%s;;",
            APP_WIFI_AP_SSID,
            APP_WIFI_AP_PASSWORD
        );

    if (
        length <= 0 ||
        length >= (int)sizeof(payload)
    ) {
        return;
    }

    lv_obj_t *qr =
        lv_qrcode_create(
            screen
        );

    if (qr == NULL) {
        return;
    }

    lv_qrcode_set_size(
        qr,
        70
    );

    lv_qrcode_set_dark_color(
        qr,
        lv_color_hex(0x000000)
    );

    lv_qrcode_set_light_color(
        qr,
        lv_color_hex(0xFFFFFF)
    );

    (void)lv_qrcode_update(
        qr,
        payload,
        strlen(payload)
    );

    lv_obj_set_pos(
        qr,
        238,
        42
    );

    lv_obj_set_style_border_color(
        qr,
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_set_style_border_width(
        qr,
        2,
        0
    );
#else
    (void)screen;
#endif
}


void ui_render_wifi_locked(void)
{
    ui_context_t *context =
        ui_context();

    /*
     * The third action row reaches y=213. Command feedback therefore
     * belongs in the fixed footer instead of being painted over the
     * last row.
     */
    const char *footer =
        context->notice[0] != '\0'
            ? context->notice
            : "UP/DOWN NAV   OK SELECT   LEFT BACK";

    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "WI-FI / WEB",
            footer
        );

    if (screen == NULL) {
        return;
    }

    const bool setup =
        wifi_manager_is_setup_mode();

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            10,
            42,
            setup
                ? 220
                : 300,
            70
        );

    if (card == NULL) {
        return;
    }

    char ssid[40] = {0};
    char ip[24] = {0};
    char text[96];

    snprintf(
        text,
        sizeof(text),
        "MODE  %s",
        setup
            ? "SETUP AP"
            : "STATION"
    );

    ui_wifi_label(
        card,
        10,
        8,
        setup
            ? 200
            : 280,
        text,
        setup
            ? UI_WIFI_COLOR_WARNING
            : UI_WIFI_COLOR_ACCENT
    );

    if (setup) {
        snprintf(
            text,
            sizeof(text),
            "AP  %s",
            APP_WIFI_AP_SSID
        );

        ui_wifi_label(
            card,
            10,
            29,
            200,
            text,
            UI_WIFI_COLOR_TEXT
        );

        ui_wifi_label(
            card,
            10,
            49,
            200,
            APP_WIFI_SETUP_URL,
            UI_WIFI_COLOR_MUTED
        );

        ui_wifi_render_setup_qr(
            screen
        );
    } else {
        if (
            wifi_manager_get_saved_ssid(
                ssid,
                sizeof(ssid)
            ) != ESP_OK ||
            ssid[0] == '\0'
        ) {
            snprintf(
                ssid,
                sizeof(ssid),
                "--"
            );
        }

        if (
            wifi_manager_sta_ip_string(
                ip,
                sizeof(ip)
            ) != ESP_OK ||
            ip[0] == '\0'
        ) {
            snprintf(
                ip,
                sizeof(ip),
                "--"
            );
        }

        snprintf(
            text,
            sizeof(text),
            "SSID  %s",
            ssid
        );

        ui_wifi_label(
            card,
            10,
            29,
            280,
            text,
            UI_WIFI_COLOR_TEXT
        );

        snprintf(
            text,
            sizeof(text),
            "IP    %s",
            ip
        );

        ui_wifi_label(
            card,
            10,
            49,
            280,
            text,
            ip[0] != '-'
                ? UI_WIFI_COLOR_GOOD
                : UI_WIFI_COLOR_MUTED
        );
    }

    static const char *actions[3] = {
        "ENTER SETUP MODE",
        "CONNECT SAVED NETWORK",
        "FORGET SAVED NETWORK",
    };

    for (size_t i = 0; i < 3; ++i) {
        ui_create_row_locked(
            screen,
            118 + (int)(i * 32U),
            actions[i],
            i == context->wifi_action
        );
    }
}
