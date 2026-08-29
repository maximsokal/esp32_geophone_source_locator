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
        *value =
            (*value + 1U) %
            count;
    } else if (*value == 0) {
        *value =
            count - 1U;
    } else {
        (*value)--;
    }
}


void ui_open_home_locked(void)
{
    ui_context_t *context =
        ui_context();

    context->screen =
        UI_SCREEN_HOME;

    context->selected =
        0;

    context->storage_erase_confirm =
        false;

    ui_set_notice_locked(
        NULL
    );

    ui_render_home_locked();
}


static void ui_open_home_selection_locked(void)
{
    ui_context_t *context =
        ui_context();

    static const ui_screen_t screens[5] = {
        UI_SCREEN_OBJECTS,
        UI_SCREEN_MEASUREMENTS,
        UI_SCREEN_STORAGE,
        UI_SCREEN_WIFI,
        UI_SCREEN_SYSTEM,
    };

    if (context->selected >= 5) {
        context->selected =
            0;
    }

    context->screen =
        screens[context->selected];

    context->selected =
        0;

    ui_set_notice_locked(
        NULL
    );

    if (context->screen == UI_SCREEN_OBJECTS) {
        context->objects_mode =
            UI_OBJECTS_MENU;

        context->object_action =
            0;
    } else if (context->screen == UI_SCREEN_STORAGE) {
        context->storage_action =
            0;

        context->storage_erase_confirm =
            false;

        (void)storage_service_info_async();
    } else if (context->screen == UI_SCREEN_WIFI) {
        context->wifi_action =
            0;
    }

    ui_render_current_locked();
}


static void ui_handle_home_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (key == KEYPAD_KEY_UP) {
        ui_move_index(
            &context->selected,
            5,
            false
        );

        ui_render_home_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(
            &context->selected,
            5,
            true
        );

        ui_render_home_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        ui_open_home_selection_locked();
    }
}


void ui_dispatch_key_locked(
    keypad_key_t key
)
{
    switch (ui_context()->screen) {
        case UI_SCREEN_HOME:
            ui_handle_home_key_locked(
                key
            );
            break;

        case UI_SCREEN_OBJECTS:
            ui_handle_objects_key_locked(
                key
            );
            break;

        case UI_SCREEN_STORAGE:
            ui_handle_storage_key_locked(
                key
            );
            break;

        case UI_SCREEN_WIFI:
            ui_handle_wifi_key_locked(
                key
            );
            break;

        case UI_SCREEN_MEASUREMENTS:
        case UI_SCREEN_SYSTEM:
            if (key == KEYPAD_KEY_LEFT) {
                ui_open_home_locked();
            }
            break;

        default:
            break;
    }
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

            if (
                ui_context()->screen ==
                UI_SCREEN_WIFI
            ) {
                ui_render_wifi_locked();
            }
            break;

        case APP_EVENT_KEYPAD:
        default:
            break;
    }
}
