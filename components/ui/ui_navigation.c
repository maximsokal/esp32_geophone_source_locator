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
    switch ((app_event_type_t)event_type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
        case APP_EVENT_WIFI_STA_CONNECTED:
        case APP_EVENT_WIFI_STA_DISCONNECTED:
            ui_refresh_header_locked();

            if (ui_context()->screen == UI_SCREEN_WIFI) {
                ui_screen_render_locked(UI_SCREEN_WIFI);
            }
            break;

        case APP_EVENT_KEYPAD:
        default:
            break;
    }
}
