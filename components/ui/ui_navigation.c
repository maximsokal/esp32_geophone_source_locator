#include "ui_internal.h"

#include "app_event_bus.h"


void ui_move_index(
    size_t *value,
    size_t count,
    bool down
)
{
    if (
        value == NULL ||
        count == 0
    ) {
        return;
    }

    if (down) {
        *value = (*value + 1U) % count;
    } else if (*value == 0) {
        *value = count - 1U;
    } else {
        (*value)--;
    }
}


void ui_open_home_locked(void)
{
    ui_screen_open_locked(UI_SCREEN_HOME);
}


void ui_dispatch_key_locked(
    keypad_key_t key
)
{
    ui_screen_handle_key_locked(
        ui_context()->screen,
        key
    );
}


void ui_dispatch_app_event_locked(
    int event_type,
    int32_t event_value
)
{
    ui_context_t *context = ui_context();

    switch ((app_event_type_t)event_type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                "PHONE CONNECTED - SCAN ROOT CA QR"
            );
            break;

        case APP_EVENT_WIFI_SETUP_CLIENT_DISCONNECTED:
            context->wifi_setup_page_opened = false;

            if (context->wifi_credentials_saved) {
                ui_set_notice_locked(
                    "SETUP LINK CLOSED - SWITCHING TO WI-FI"
                );
            } else if (
                context->wifi_ca_downloaded &&
                !context->wifi_ca_install_confirmed
            ) {
                ui_set_notice_locked(
                    "INSTALL CA, RECONNECT, THEN CONFIRM"
                );
            } else {
                ui_set_notice_locked(
                    "PHONE DISCONNECTED - SCAN WI-FI QR"
                );
            }
            break;

        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
            context->wifi_setup_page_opened = true;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                "SETUP PAGE OPEN - ENTER HOTSPOT DATA"
            );
            break;

        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
            context->wifi_credentials_saved = true;
            ui_set_notice_locked(
                "SAVED - ENABLE HOTSPOT, CONNECTING..."
            );
            break;

        case APP_EVENT_WIFI_STA_CONNECTING:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            context->wifi_verify_ready = false;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = 0;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                "CONNECTING TO SAVED WI-FI..."
            );
            break;

        case APP_EVENT_WIFI_STA_CONNECTED:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                "WI-FI CONNECTED - STARTING HTTPS"
            );
            break;

        case APP_EVENT_WIFI_STA_DISCONNECTED:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            context->wifi_verify_ready = false;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = 0;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                "WI-FI DISCONNECTED - RETRYING"
            );
            break;

        case APP_EVENT_TLS_CA_DOWNLOADED:
            context->wifi_ca_downloaded = true;
            context->wifi_ca_install_confirmed = false;
            ui_set_notice_locked(
                "CA DOWNLOADED - INSTALL IT, THEN CONTINUE"
            );
            break;

        case APP_EVENT_TLS_CA_INSTALL_CONFIRMED:
            context->wifi_ca_downloaded = true;
            context->wifi_ca_install_confirmed = true;
            ui_set_notice_locked(
                "CA CONFIRMED - SCAN HOTSPOT SETUP QR"
            );
            break;

        case APP_EVENT_TLS_VERIFY_READY:
            context->wifi_verify_ready = true;
            context->wifi_verify_result_received = false;
            context->wifi_verify_nonce = (uint32_t)event_value;
            context->wifi_verify_result_mask = 0;
            ui_set_notice_locked(
                "HTTPS READY - SCAN VERIFY QR ON PHONE"
            );
            break;

        case APP_EVENT_TLS_VERIFY_RESULT:
            context->wifi_verify_result_received = true;
            context->wifi_verify_result_mask = (uint32_t)event_value;

            if (
                (context->wifi_verify_result_mask &
                 APP_TLS_VERIFY_REQUIRED_MASK) ==
                APP_TLS_VERIFY_REQUIRED_MASK
            ) {
                ui_set_notice_locked(
                    "HTTPS/CAMERA/WEBXR VERIFIED"
                );
            } else {
                ui_set_notice_locked(
                    "VERIFY INCOMPLETE - CHECK PHONE RESULTS"
                );
            }
            break;

        case APP_EVENT_KEYPAD:
        default:
            return;
    }

    ui_refresh_header_locked();

    if (context->screen == UI_SCREEN_WIFI) {
        ui_screen_render_locked(UI_SCREEN_WIFI);
    }
}
