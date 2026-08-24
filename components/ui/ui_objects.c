#include "ui_internal.h"

#include "ui_objects_internal.h"


esp_err_t ui_objects_open(void)
{
    ESP_ERROR_CHECK(
        object_service_set_event_callback(
            ui_objects_service_event,
            NULL
        )
    );

    ui_objects_state_t *state =
        ui_objects_state();

    state->mode =
        OBJECT_MODE_MENU;

    state->selected_action =
        0;

    state->notice[0] =
        '\0';

    return
        ui_objects_draw_menu_internal();
}


esp_err_t ui_objects_draw(void)
{
    ui_objects_state_t *state =
        ui_objects_state();

    switch (state->mode) {
        case OBJECT_MODE_SELECT_ACTIVE:
        case OBJECT_MODE_SELECT_DELETE:
            return
                ui_objects_draw_current_list_internal();

        case OBJECT_MODE_DELETE_CONFIRM:
            return
                ui_objects_view_draw_delete_confirm(
                    &state->objects[
                        state->selected_object
                    ]
                );

        case OBJECT_MODE_MENU:
        default:
            return
                ui_objects_draw_menu_internal();
    }
}


esp_err_t ui_objects_handle_key(
    keypad_key_t key
)
{
    if (
        object_service_is_busy()
    ) {
        return ESP_OK;
    }

    ui_objects_state_t *state =
        ui_objects_state();

    switch (state->mode) {
        case OBJECT_MODE_SELECT_ACTIVE:
        case OBJECT_MODE_SELECT_DELETE:
            return
                ui_objects_handle_list_key_internal(
                    key
                );

        case OBJECT_MODE_DELETE_CONFIRM:
            return
                ui_objects_handle_delete_confirm_internal(
                    key
                );

        case OBJECT_MODE_MENU:
        default:
            return
                ui_objects_handle_menu_key_internal(
                    key
                );
    }
}
