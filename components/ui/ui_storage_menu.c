#include "ui_storage_internal.h"

#include "ui_storage_view.h"


static esp_err_t ui_storage_move_action(
    keypad_key_t key
)
{
    ui_storage_state_t *state =
        ui_storage_state();

    size_t previous =
        state->selected_action;

    if (
        key ==
        KEYPAD_KEY_DOWN
    ) {
        state->selected_action =
            (
                state->selected_action +
                1
            ) %
            UI_STORAGE_ACTION_COUNT;
    }
    else {
        state->selected_action =
            state->selected_action == 0
            ? UI_STORAGE_ACTION_COUNT - 1
            : state->selected_action - 1;
    }

    return
        ui_storage_view_update_selection(
            previous,
            state->selected_action
        );
}


esp_err_t ui_storage_handle_menu_key(
    keypad_key_t key
)
{
    ui_storage_state_t *state =
        ui_storage_state();

    switch (key) {
        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
            return
                ui_storage_move_action(
                    key
                );

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            switch (
                state->selected_action
            ) {
                case 0:
                    return
                        ui_storage_request_info(
                            STORAGE_MODE_CARD_INFO
                        );

                case 1:
                    return
                        ui_storage_request_info(
                            STORAGE_MODE_FREE_SPACE
                        );

                case 2:
                    state->mode =
                        STORAGE_MODE_ERASE_CONFIRM;

                    return
                        ui_storage_draw_current_internal();

                default:
                    return ESP_ERR_INVALID_STATE;
            }

        case KEYPAD_KEY_LEFT:
            return ESP_ERR_NOT_SUPPORTED;

        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}
