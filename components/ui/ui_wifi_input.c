#include "ui_internal.h"

#include "wifi_manager.h"


static bool ui_wifi_saved_network_available(void)
{
    char ssid[40] = {0};

    return
        wifi_manager_get_saved_ssid(
            ssid,
            sizeof(ssid)
        ) == ESP_OK &&
        ssid[0] != '\0';
}


void ui_handle_wifi_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context =
        ui_context();

    if (key == KEYPAD_KEY_LEFT) {
        context->wifi_confirm =
            UI_WIFI_CONFIRM_NONE;

        ui_open_home_locked();
    } else if (key == KEYPAD_KEY_UP) {
        context->wifi_confirm =
            UI_WIFI_CONFIRM_NONE;

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
        context->wifi_confirm =
            UI_WIFI_CONFIRM_NONE;

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
        const bool setup =
            wifi_manager_is_setup_mode();

        const bool has_saved_network =
            ui_wifi_saved_network_available();

        esp_err_t err;

        if (context->wifi_action == 0) {
            if (setup) {
                context->wifi_confirm =
                    UI_WIFI_CONFIRM_NONE;
                return;
            }

            if (
                context->wifi_confirm !=
                UI_WIFI_CONFIRM_SETUP
            ) {
                context->wifi_confirm =
                    UI_WIFI_CONFIRM_SETUP;

                ui_set_notice_locked(
                    "PRESS OK AGAIN: ENTER SETUP"
                );

                ui_render_wifi_locked();
                return;
            }

            context->wifi_confirm =
                UI_WIFI_CONFIRM_NONE;

            context->wifi_setup_page_opened =
                false;

            context->wifi_credentials_saved =
                false;

            err =
                wifi_manager_enter_setup_mode();
        } else if (context->wifi_action == 1) {
            if (!has_saved_network) {
                context->wifi_confirm =
                    UI_WIFI_CONFIRM_NONE;
                return;
            }

            context->wifi_confirm =
                UI_WIFI_CONFIRM_NONE;

            context->wifi_setup_page_opened =
                false;

            context->wifi_credentials_saved =
                false;

            err =
                wifi_manager_connect_saved();
        } else {
            if (!has_saved_network) {
                context->wifi_confirm =
                    UI_WIFI_CONFIRM_NONE;
                return;
            }

            if (
                context->wifi_confirm !=
                UI_WIFI_CONFIRM_FORGET
            ) {
                context->wifi_confirm =
                    UI_WIFI_CONFIRM_FORGET;

                ui_set_notice_locked(
                    "PRESS OK AGAIN: FORGET NETWORK"
                );

                ui_render_wifi_locked();
                return;
            }

            context->wifi_confirm =
                UI_WIFI_CONFIRM_NONE;

            context->wifi_setup_page_opened =
                false;

            context->wifi_credentials_saved =
                false;

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
