#include "ui_objects_internal.h"

#include <stdio.h>


static void ui_objects_event_finish_menu(
    const char *prefix,
    const char *object_id,
    bool warning
)
{
    ui_objects_state_t *state =
        ui_objects_state();

    if (object_id != NULL) {
        snprintf(
            state->notice,
            sizeof(state->notice),
            "%s %s",
            prefix,
            object_id
        );
    }
    else {
        snprintf(
            state->notice,
            sizeof(state->notice),
            "%s",
            prefix
        );
    }

    state->notice_warning =
        warning;

    state->mode =
        OBJECT_MODE_MENU;

    ui_objects_report_draw_error(
        ui_objects_draw_menu_internal()
    );
}


void ui_objects_service_event(
    const object_service_event_t *event,
    void *user_context
)
{
    (void)user_context;

    if (event == NULL) {
        return;
    }

    switch (event->type) {
        case OBJECT_SERVICE_EVENT_CREATE_DONE:
            ui_objects_event_finish_menu(
                "CREATED",
                event->object.id,
                false
            );
            break;

        case OBJECT_SERVICE_EVENT_CREATE_FAILED:
            ui_objects_event_finish_menu(
                "CREATE FAILED",
                NULL,
                true
            );
            break;

        case OBJECT_SERVICE_EVENT_LIST_DONE:
        case OBJECT_SERVICE_EVENT_LIST_FAILED:
            ui_objects_event_handle_list(
                event
            );
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE:
            ui_objects_event_finish_menu(
                "ACTIVE",
                event->object_id,
                false
            );
            break;

        case OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED:
            ui_objects_event_finish_menu(
                "SAVE FAILED",
                NULL,
                true
            );
            break;

        case OBJECT_SERVICE_EVENT_DELETE_DONE:
            ui_objects_event_finish_menu(
                "DELETED",
                event->object_id,
                false
            );
            break;

        case OBJECT_SERVICE_EVENT_DELETE_FAILED:
            ui_objects_event_finish_menu(
                "DELETE FAILED",
                NULL,
                true
            );
            break;

        default:
            break;
    }
}
