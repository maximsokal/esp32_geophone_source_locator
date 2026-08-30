#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "wifi_manager.h"


static void ui_wifi_render_qr(
    lv_obj_t *screen,
    const char *payload
)
{
#if LV_USE_QRCODE
    if (
        screen == NULL ||
        payload == NULL ||
        payload[0] == '\0'
    ) {
        return;
    }

    lv_obj_t *qr = lv_qrcode_create(screen);
    if (qr == NULL) {
        return;
    }

    lv_qrcode_set_size(qr, 70);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    (void)lv_qrcode_update(qr, payload, strlen(payload));

    lv_obj_set_pos(qr, 238, 42);
    lv_obj_set_style_border_color(qr, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(qr, 2, 0);
#else
    (void)screen;
    (void)payload;
#endif
}


static void ui_wifi_render_join_qr(
    lv_obj_t *screen
)
{
    char payload[128];

    const int length = snprintf(
        payload,
        sizeof(payload),
        "WIFI:T:WPA;S:%s;P:%s;;",
        APP_WIFI_AP_SSID,
        APP_WIFI_AP_PASSWORD
    );

    if (length <= 0 || length >= (int)sizeof(payload)) {
        return;
    }

    ui_wifi_render_qr(screen, payload);
}


static void ui_wifi_render_setup_card(
    lv_obj_t *screen
)
{
    ui_context_t *context = ui_context();

    const bool client_ready =
        wifi_manager_setup_client_connected();

    const bool show_page_qr =
        client_ready &&
        !context->wifi_credentials_saved;

    lv_obj_t *card = ui_create_card_locked(
        screen,
        10,
        42,
        show_page_qr || !client_ready ? 220 : 300,
        70
    );

    if (card == NULL) {
        return;
    }

    char text[96];

    if (context->wifi_credentials_saved) {
        (void)ui_create_label_locked(
            card,
            10,
            8,
            280,
            "SETTINGS SAVED",
            UI_COLOR_GOOD
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            280,
            "ENABLE PHONE HOTSPOT",
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            280,
            "GEOLOCATOR IS SWITCHING TO STATION MODE",
            UI_COLOR_MUTED
        );

        return;
    }

    if (!client_ready) {
        (void)ui_create_label_locked(
            card,
            10,
            8,
            200,
            "STEP 1/2  CONNECT PHONE",
            UI_COLOR_WARNING
        );

        snprintf(
            text,
            sizeof(text),
            "AP  %s",
            APP_WIFI_AP_SSID
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            200,
            text,
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            200,
            "SCAN WI-FI QR",
            UI_COLOR_MUTED
        );

        ui_wifi_render_join_qr(screen);
        return;
    }

    (void)ui_create_label_locked(
        card,
        10,
        8,
        200,
        context->wifi_setup_page_opened
            ? "SETUP PAGE OPENED"
            : "STEP 2/2  OPEN SETUP PAGE",
        context->wifi_setup_page_opened
            ? UI_COLOR_GOOD
            : UI_COLOR_ACCENT
    );

    (void)ui_create_label_locked(
        card,
        10,
        29,
        200,
        "PHONE CONNECTED",
        UI_COLOR_GOOD
    );

    (void)ui_create_label_locked(
        card,
        10,
        49,
        200,
        APP_WIFI_SETUP_URL,
        UI_COLOR_MUTED
    );

    ui_wifi_render_qr(
        screen,
        APP_WIFI_SETUP_URL
    );
}


void ui_render_wifi_locked(void)
{
    ui_context_t *context = ui_context();

    /*
     * The last action row ends immediately above the fixed footer.
     * Command/setup feedback therefore replaces footer help text rather
     * than overlapping a selectable row.
     */
    const char *footer =
        context->notice[0] != '\0'
            ? context->notice
            : "UP/DOWN NAV   OK SELECT   LEFT BACK";

    lv_obj_t *screen = ui_create_screen_base_locked(
        "WI-FI / WEB",
        footer
    );

    if (screen == NULL) {
        return;
    }

    const bool setup = wifi_manager_is_setup_mode();

    char ssid[40] = {0};
    const bool has_saved_network =
        wifi_manager_get_saved_ssid(
            ssid,
            sizeof(ssid)
        ) == ESP_OK &&
        ssid[0] != '\0';

    if (setup) {
        ui_wifi_render_setup_card(screen);
    } else {
        lv_obj_t *card = ui_create_card_locked(
            screen,
            10,
            42,
            300,
            70
        );

        if (card == NULL) {
            return;
        }

        char ip[24] = {0};
        char text[96];

        (void)ui_create_label_locked(
            card,
            10,
            8,
            280,
            "MODE  STATION",
            UI_COLOR_ACCENT
        );

        if (!has_saved_network) {
            snprintf(ssid, sizeof(ssid), "--");
        }

        if (
            wifi_manager_sta_ip_string(ip, sizeof(ip)) != ESP_OK ||
            ip[0] == '\0'
        ) {
            snprintf(ip, sizeof(ip), "--");
        }

        snprintf(text, sizeof(text), "SSID  %s", ssid);
        (void)ui_create_label_locked(
            card, 10, 29, 280, text, UI_COLOR_TEXT
        );

        snprintf(text, sizeof(text), "IP    %s", ip);
        (void)ui_create_label_locked(
            card,
            10,
            49,
            280,
            text,
            ip[0] != '-' ? UI_COLOR_GOOD : UI_COLOR_MUTED
        );
    }

    const char *actions[3] = {
        context->wifi_confirm == UI_WIFI_CONFIRM_SETUP
            ? "CONFIRM ENTER SETUP"
            : "ENTER SETUP MODE",
        "CONNECT SAVED NETWORK",
        context->wifi_confirm == UI_WIFI_CONFIRM_FORGET
            ? "CONFIRM FORGET NETWORK"
            : "FORGET SAVED NETWORK",
    };

    const bool enabled[3] = {
        !setup,
        has_saved_network,
        has_saved_network,
    };

    for (size_t i = 0; i < 3; ++i) {
        (void)ui_create_row_locked(
            screen,
            118 + (int)(i * 32U),
            actions[i],
            i == context->wifi_action,
            enabled[i]
        );
    }
}
