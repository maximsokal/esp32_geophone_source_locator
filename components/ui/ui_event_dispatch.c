#include "ui_event_dispatch.h"

#include "ui_navigation.h"
#include "ui_screen.h"
#include "ui_screen_router.h"
#include "ui_state.h"
#include "ui_wifi_events.h"


esp_err_t ui_event_dispatch_key(
    keypad_key_t key
)
{
    ui_screen_t screen =
        ui_state_screen();

    esp_err_t err =
        ui_screen_handle_key(
            screen,
            key
        );

    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (
        err ==
            ESP_ERR_NOT_SUPPORTED &&
        screen !=
            UI_SCREEN_HOME &&
        key ==
            KEYPAD_KEY_LEFT
    ) {
        return
            ui_navigation_return_home();
    }

    return err;
}


esp_err_t ui_event_dispatch_app(
    app_event_type_t type
)
{
    switch (type) {
        case APP_EVENT_WIFI_SETUP_CLIENT_READY:
        case APP_EVENT_WIFI_SETUP_PAGE_OPENED:
        case APP_EVENT_WIFI_CREDENTIALS_SAVED:
        case APP_EVENT_WIFI_STA_CONNECTED:
        case APP_EVENT_WIFI_STA_DISCONNECTED:
            return
                ui_wifi_events_handle(
                    type
                );

        case APP_EVENT_KEYPAD:
        default:
            return ESP_OK;
    }
}
