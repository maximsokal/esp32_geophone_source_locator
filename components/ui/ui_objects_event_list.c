#include "ui_objects_internal.h"

#include <stdio.h>
#include <string.h>


void ui_objects_event_handle_list(
    const object_service_event_t *event
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    if (
        event->type ==
        OBJECT_SERVICE_EVENT_LIST_FAILED
    ) {
        snprintf(
            state->notice,
            sizeof(state->notice),
            "LOAD FAILED"
        );

        state->notice_warning =
            true;

        state->mode =
            OBJECT_MODE_MENU;

        ui_objects_report_draw_error(
            ui_objects_draw_menu_internal()
        );

        return;
    }

    state->object_count =
        event->object_count;

    if (
        state->object_count >
        OBJECT_SERVICE_MAX_OBJECTS
    ) {
        state->object_count =
            OBJECT_SERVICE_MAX_OBJECTS;
    }

    memcpy(
        state->objects,
        event->objects,
        state->object_count *
            sizeof(
                object_service_summary_t
            )
    );

    if (
        state->object_count == 0
    ) {
        snprintf(
            state->notice,
            sizeof(state->notice),
            "NO OBJECTS"
        );

        state->notice_warning =
            true;

        state->mode =
            OBJECT_MODE_MENU;

        ui_objects_report_draw_error(
            ui_objects_draw_menu_internal()
        );

        return;
    }

    ui_objects_select_active_object();

    state->notice[0] =
        '\0';

    state->mode =
        state->pending_list_mode;

    ui_objects_report_draw_error(
        ui_objects_draw_current_list_internal()
    );
}
