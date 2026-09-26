#include "ui_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_events.h"
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

    const bool show_qr =
        client_ready &&
        !context->wifi_credentials_saved;

    lv_obj_t *card = ui_create_card_locked(
        screen,
        10,
        42,
        show_qr || !client_ready ? 220 : 300,
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
            ui_tr(UI_I18N_KEY_WIFI_STEP3_SAVED),
            UI_COLOR_GOOD
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            280,
            ui_tr(UI_I18N_KEY_WIFI_ENABLE_HOTSPOT),
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            280,
            ui_tr(UI_I18N_KEY_WIFI_NEXT_VERIFY),
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
            ui_tr(UI_I18N_KEY_WIFI_STEP1_CONNECT),
            UI_COLOR_WARNING
        );

        snprintf(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_WIFI_AP_SSID),
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
            ui_tr(UI_I18N_KEY_WIFI_SCAN_JOIN),
            UI_COLOR_MUTED
        );

        ui_wifi_render_join_qr(screen);
        return;
    }

    if (!context->wifi_ca_downloaded) {
        (void)ui_create_label_locked(
            card,
            10,
            8,
            200,
            ui_tr(UI_I18N_KEY_WIFI_STEP2_INSTALL),
            UI_COLOR_WARNING
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            200,
            ui_tr(UI_I18N_KEY_WIFI_DOWNLOAD_CA),
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            200,
            APP_WIFI_CA_URL,
            UI_COLOR_MUTED
        );

        ui_wifi_render_qr(
            screen,
            APP_WIFI_CA_URL
        );
        return;
    }

    if (!context->wifi_ca_install_confirmed) {
        (void)ui_create_label_locked(
            card,
            10,
            8,
            200,
            ui_tr(UI_I18N_KEY_WIFI_STEP2_DOWNLOADED),
            UI_COLOR_ACCENT
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            200,
            ui_tr(UI_I18N_KEY_WIFI_INSTALL_ANDROID),
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            200,
            ui_tr(UI_I18N_KEY_WIFI_RETURN_CONTINUE),
            UI_COLOR_MUTED
        );

        ui_wifi_render_qr(
            screen,
            APP_WIFI_CA_URL
        );
        return;
    }

    (void)ui_create_label_locked(
        card,
        10,
        8,
        200,
        context->wifi_setup_page_opened
            ? ui_tr(UI_I18N_KEY_WIFI_STEP3_PAGE_OPEN)
            : ui_tr(UI_I18N_KEY_WIFI_STEP3_CONFIGURE),
        context->wifi_setup_page_opened
            ? UI_COLOR_GOOD
            : UI_COLOR_ACCENT
    );

    (void)ui_create_label_locked(
        card,
        10,
        29,
        200,
        ui_tr(UI_I18N_KEY_WIFI_ROOT_CA_CONFIRMED),
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


static void ui_wifi_render_sta_card(
    lv_obj_t *screen,
    const char *ssid,
    const char *ip
)
{
    ui_context_t *context = ui_context();

    const bool show_verify_qr =
        context->wifi_verify_ready &&
        !context->wifi_verify_result_received &&
        context->wifi_verify_nonce != 0;

    lv_obj_t *card = ui_create_card_locked(
        screen,
        10,
        42,
        show_verify_qr ? 220 : 300,
        70
    );

    if (card == NULL) {
        return;
    }

    if (context->wifi_verify_result_received) {
        const uint32_t mask =
            context->wifi_verify_result_mask;

        const bool ready =
            (mask & APP_TLS_VERIFY_REQUIRED_MASK) ==
            APP_TLS_VERIFY_REQUIRED_MASK;

        (void)ui_create_label_locked(
            card,
            10,
            8,
            280,
            ready
                ? ui_tr(UI_I18N_KEY_WIFI_STEP4_VERIFIED)
                : ui_tr(UI_I18N_KEY_WIFI_STEP4_INCOMPLETE),
            ready
                ? UI_COLOR_GOOD
                : UI_COLOR_WARNING
        );

        char result[96];

        snprintf(
            result,
            sizeof(result),
            ui_tr(UI_I18N_KEY_WIFI_VERIFY_CODES),
            (mask & APP_TLS_VERIFY_SECURE_CONTEXT) ? "OK" : "--",
            (mask & APP_TLS_VERIFY_CAMERA) ? "OK" : "--",
            (mask & APP_TLS_VERIFY_XR_API) ? "OK" : "--",
            (mask & APP_TLS_VERIFY_IMMERSIVE_AR) ? "OK" : "--"
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            280,
            result,
            ready ? UI_COLOR_GOOD : UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            280,
            ui_tr(UI_I18N_KEY_WIFI_PHONE_REPORT),
            UI_COLOR_MUTED
        );

        return;
    }

    if (show_verify_qr) {
        (void)ui_create_label_locked(
            card,
            10,
            8,
            200,
            ui_tr(UI_I18N_KEY_WIFI_VERIFY_HTTPS),
            UI_COLOR_WARNING
        );

        (void)ui_create_label_locked(
            card,
            10,
            29,
            200,
            ui_tr(UI_I18N_KEY_WIFI_SCAN_PHONE),
            UI_COLOR_TEXT
        );

        (void)ui_create_label_locked(
            card,
            10,
            49,
            200,
            ui_tr(UI_I18N_KEY_WIFI_CAMERA_WEBXR),
            UI_COLOR_MUTED
        );

        char verify_url[128];

        const int length = snprintf(
            verify_url,
            sizeof(verify_url),
            "%s?t=%08" PRIx32,
            APP_HTTPS_VERIFY_URL,
            context->wifi_verify_nonce
        );

        if (
            length > 0 &&
            length < (int)sizeof(verify_url)
        ) {
            ui_wifi_render_qr(
                screen,
                verify_url
            );
        }

        return;
    }

    char text[96];

    snprintf(text, sizeof(text), ui_tr(UI_I18N_KEY_WIFI_MODE), "STATION");
    (void)ui_create_label_locked(
        card,
        10,
        8,
        280,
        text,
        UI_COLOR_ACCENT
    );

    snprintf(text, sizeof(text), ui_tr(UI_I18N_KEY_WIFI_SSID), ssid);
    (void)ui_create_label_locked(
        card,
        10,
        29,
        280,
        text,
        UI_COLOR_TEXT
    );

    snprintf(text, sizeof(text), ui_tr(UI_I18N_KEY_WIFI_IP), ip);
    (void)ui_create_label_locked(
        card,
        10,
        49,
        280,
        text,
        ip[0] != '-' ? UI_COLOR_GOOD : UI_COLOR_MUTED
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
            : ui_tr(UI_I18N_KEY_SHARED_FOOTER_BACK);

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_WIFI_TITLE),
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
        char ip[24] = {0};

        if (!has_saved_network) {
            snprintf(ssid, sizeof(ssid), "%s", ui_tr(UI_I18N_KEY_SHARED_EMPTY));
        }

        if (
            wifi_manager_sta_ip_string(ip, sizeof(ip)) != ESP_OK ||
            ip[0] == '\0'
        ) {
            snprintf(ip, sizeof(ip), "%s", ui_tr(UI_I18N_KEY_SHARED_EMPTY));
        }

        ui_wifi_render_sta_card(
            screen,
            ssid,
            ip
        );
    }

    const ui_i18n_key_t actions[3] = {
        context->wifi_confirm == UI_WIFI_CONFIRM_SETUP
            ? UI_I18N_KEY_WIFI_SETUP_CONFIRM
            : UI_I18N_KEY_WIFI_SETUP_ENTER,
        UI_I18N_KEY_WIFI_CONNECT_SAVED,
        context->wifi_confirm == UI_WIFI_CONFIRM_FORGET
            ? UI_I18N_KEY_WIFI_FORGET_CONFIRM
            : UI_I18N_KEY_WIFI_FORGET,
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
            ui_tr(actions[i]),
            i == context->wifi_action,
            enabled[i]
        );
    }
}
