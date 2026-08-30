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
    int event_type
)
{
    ui_context_t *context = ui_context();

    switch ((app_event_type_t)event_type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                "PHONE CONNECTED - SCAN SETUP PAGE QR"
            );
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

        case APP_EVENT_WIFI_STA_CONNECTED:
            context->wifi_setup_page_opened = false;
            context->wifi_credentials_saved = false;
            ui_set_notice_locked(
                "WI-FI CONNECTED"
            );
            break;

        case APP_EVENT_WIFI_STA_DISCONNECTED:
            /*
             * Keep the setup/save instruction visible while the station
             * retries a phone hotspot that may not have been enabled yet.
             */
            if (!context->wifi_credentials_saved) {
                ui_set_notice_locked(
                    "WI-FI DISCONNECTED - RETRYING"
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
