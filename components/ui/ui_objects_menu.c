#include "ui_objects_internal.h"

#include <stdio.h>


static esp_err_t ui_objects_create(void)
{
    ui_objects_state_t *state =
        ui_objects_state();

    snprintf(
        state->notice,
        sizeof(state->notice),
        "CREATING..."
    );

    state->notice_warning =
        false;

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            state->notice,
            false
        )
    );

    return
        object_service_create_async();
}


static esp_err_t ui_objects_load_list(
    object_mode_t target_mode
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    state->pending_list_mode =
        target_mode;

    snprintf(
        state->notice,
        sizeof(state->notice),
        "LOADING..."
    );

    state->notice_warning =
        false;

    ESP_ERROR_CHECK(
        ui_objects_view_draw_notice(
            state->notice,
            false
        )
    );

    return
        object_service_list_async();
}


static esp_err_t ui_objects_move_action(
    keypad_key_t key
)
{
    ui_objects_state_t *state =
        ui_objects_state();

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
            UI_OBJECTS_ACTION_COUNT;
    }
    else {
        state->selected_action =
            state->selected_action == 0
            ? UI_OBJECTS_ACTION_COUNT - 1
            : state->selected_action - 1;
    }

    return
        ui_objects_view_update_menu_selection(
            previous,
            state->selected_action
        );
}


esp_err_t ui_objects_handle_menu_key_internal(
    keypad_key_t key
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    switch (key) {
        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
            return
                ui_objects_move_action(
                    key
                );

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            switch (
                state->selected_action
            ) {
                case 0:
                    return
                        ui_objects_create();

                case 1:
                    return
                        ui_objects_load_list(
                            OBJECT_MODE_SELECT_ACTIVE
                        );

                case 2:
                    return
                        ui_objects_load_list(
                            OBJECT_MODE_SELECT_DELETE
                        );

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
