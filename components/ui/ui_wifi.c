#include "ui_internal.h"

#include <stdio.h>

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

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_width(
        label,
        width
    );

    lv_label_set_text(
        label,
        text
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


void ui_render_wifi_locked(void)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "WI-FI / WEB",
            "UP/DOWN NAV   OK SELECT   LEFT BACK"
        );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            10,
            42,
            300,
            72
        );

    char ssid[40] = {0};
    char ip[24] = {0};
    char text[96];

    const bool setup =
        wifi_manager_is_setup_mode();

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
        280,
        text,
        setup
            ? UI_WIFI_COLOR_WARNING
            : UI_WIFI_COLOR_ACCENT
    );

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
        30,
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
        50,
        280,
        text,
        ip[0] != '-' ?
            UI_WIFI_COLOR_GOOD :
            UI_WIFI_COLOR_MUTED
    );

    static const char *actions[3] = {
        "ENTER SETUP MODE",
        "CONNECT SAVED NETWORK",
        "FORGET SAVED NETWORK",
    };

    for (size_t i = 0; i < 3; ++i) {
        ui_create_row_locked(
            screen,
            120 + (int)(i * 30U),
            actions[i],
            i == context->wifi_action
        );
    }

    if (context->notice[0] != '\0') {
        ui_wifi_label(
            screen,
            12,
            204,
            296,
            context->notice,
            UI_WIFI_COLOR_WARNING
        );
    }
}
