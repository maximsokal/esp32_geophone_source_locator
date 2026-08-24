#include "ui_storage_internal.h"


static esp_err_t ui_storage_handle_confirm(
    keypad_key_t key
)
{
    ui_storage_state_t *state =
        ui_storage_state();

    switch (key) {
        case KEYPAD_KEY_LEFT:
            state->mode =
                STORAGE_MODE_MENU;

            return
                ui_storage_draw_menu_internal();

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            return
                ui_storage_start_erase();

        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


esp_err_t ui_storage_handle_input(
    keypad_key_t key
)
{
    if (
        storage_service_is_busy()
    ) {
        return ESP_OK;
    }

    ui_storage_state_t *state =
        ui_storage_state();

    if (
        state->mode ==
        STORAGE_MODE_MENU
    ) {
        return
            ui_storage_handle_menu_key(
                key
            );
    }

    if (
        state->mode ==
        STORAGE_MODE_ERASE_CONFIRM
    ) {
        return
            ui_storage_handle_confirm(
                key
            );
    }

    if (
        key ==
        KEYPAD_KEY_LEFT
    ) {
        state->mode =
            STORAGE_MODE_MENU;

        return
            ui_storage_draw_menu_internal();
    }

    return ESP_OK;
}
