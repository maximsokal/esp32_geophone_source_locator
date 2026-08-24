#include "ui_objects_internal.h"

#include <stddef.h>

#include "object_service.h"


esp_err_t ui_objects_draw_menu_internal(void)
{
    ui_objects_state_t *state =
        ui_objects_state();

    object_service_summary_t active;

    const object_service_summary_t *active_ptr =
        object_service_get_active(
            &active
        ) == ESP_OK
        ? &active
        : NULL;

    return
        ui_objects_view_draw_menu(
            state->selected_action,
            active_ptr,
            state->notice,
            state->notice_warning
        );
}


esp_err_t ui_objects_draw_current_list_internal(void)
{
    ui_objects_state_t *state =
        ui_objects_state();

    return
        ui_objects_view_draw_list(
            state->objects,
            state->object_count,
            state->selected_object,
            ui_objects_current_list_kind()
        );
}
