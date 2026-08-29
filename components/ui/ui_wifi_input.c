#include "ui_internal.h"

#include "wifi_manager.h"


void ui_handle_wifi_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (key == KEYPAD_KEY_LEFT) {
        ui_open_home_locked();
    } else if (key == KEYPAD_KEY_UP) {
        ui_move_index(
            &context->wifi_action,
            3,
            false
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_wifi_locked();
    } else if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(
            &context->wifi_action,
            3,
            true
        );

        ui_set_notice_locked(
            NULL
        );

        ui_render_wifi_locked();
    } else if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        esp_err_t err;

        if (context->wifi_action == 0) {
            err =
                wifi_manager_enter_setup_mode();
        } else if (context->wifi_action == 1) {
            err =
                wifi_manager_connect_saved();
        } else {
            err =
                wifi_manager_forget_sta_credentials();
        }

        ui_set_notice_locked(
            err == ESP_OK
                ? "WI-FI COMMAND ACCEPTED"
                : "WI-FI COMMAND FAILED"
        );

        ui_render_wifi_locked();
    }
}
