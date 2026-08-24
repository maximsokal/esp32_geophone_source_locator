#include "ui_objects_internal.h"

#include "ui_internal.h"


static esp_err_t ui_objects_move_selection(
    bool down
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    if (state->object_count == 0) {
        return ESP_OK;
    }

    size_t previous =
        state->selected_object;

    if (down) {
        state->selected_object++;

        if (
            state->selected_object >=
            state->object_count
        ) {
            state->selected_object =
                0;
        }
    }
    else if (
        state->selected_object == 0
    ) {
        state->selected_object =
            state->object_count - 1;
    }
    else {
        state->selected_object--;
    }

    return
        ui_objects_view_update_list_selection(
            state->objects,
            state->object_count,
            previous,
            state->selected_object,
            ui_objects_current_list_kind()
        );
}


esp_err_t ui_objects_handle_list_key_internal(
    keypad_key_t key
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    switch (key) {
        case KEYPAD_KEY_UP:
            return
                ui_objects_move_selection(
                    false
                );

        case KEYPAD_KEY_DOWN:
            return
                ui_objects_move_selection(
                    true
                );

        case KEYPAD_KEY_LEFT:
            state->mode =
                OBJECT_MODE_MENU;

            return
                ui_objects_draw_menu_internal();

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            if (
                state->object_count == 0
            ) {
                return ESP_OK;
            }

            if (
                state->mode ==
                OBJECT_MODE_SELECT_DELETE
            ) {
                state->mode =
                    OBJECT_MODE_DELETE_CONFIRM;

                return
                    ui_objects_view_draw_delete_confirm(
                        &state->objects[
                            state->selected_object
                        ]
                    );
            }

            ESP_ERROR_CHECK(
                ui_common_draw_footer(
                    "SAVING ACTIVE OBJECT..."
                )
            );

            return
                object_service_set_active_async(
                    state->objects[
                        state->selected_object
                    ].id
                );

        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}


esp_err_t ui_objects_handle_delete_confirm_internal(
    keypad_key_t key
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    switch (key) {
        case KEYPAD_KEY_LEFT:
            state->mode =
                OBJECT_MODE_SELECT_DELETE;

            return
                ui_objects_draw_current_list_internal();

        case KEYPAD_KEY_OK:
        case KEYPAD_KEY_RIGHT:
            ESP_ERROR_CHECK(
                ui_common_draw_footer(
                    "DELETING OBJECT..."
                )
            );

            return
                object_service_delete_async(
                    state->objects[
                        state->selected_object
                    ].id
                );

        case KEYPAD_KEY_UP:
        case KEYPAD_KEY_DOWN:
        case KEYPAD_KEY_NONE:
        default:
            return ESP_OK;
    }
}
