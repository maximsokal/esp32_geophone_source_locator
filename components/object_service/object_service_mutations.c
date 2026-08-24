#include "object_service_internal.h"

#include <stdio.h>
#include <string.h>

#include "object_store.h"

#include "esp_log.h"


static const char *TAG =
    "OBJECT_SERVICE";


void object_service_internal_process_mutation(
    const object_service_command_t *command
)
{
    object_service_state_t *state =
        object_service_internal_state();

    memset(
        &state->event,
        0,
        sizeof(state->event)
    );

    snprintf(
        state->event.object_id,
        sizeof(state->event.object_id),
        "%s",
        command->object_id
    );

    bool deleting =
        command->type ==
        OBJECT_SERVICE_COMMAND_DELETE;

    state->event.result =
        deleting
        ? object_store_delete(
            command->object_id
        )
        : object_store_set_active(
            command->object_id
        );

    if (deleting) {
        state->event.type =
            state->event.result == ESP_OK
            ? OBJECT_SERVICE_EVENT_DELETE_DONE
            : OBJECT_SERVICE_EVENT_DELETE_FAILED;
    }
    else {
        state->event.type =
            state->event.result == ESP_OK
            ? OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE
            : OBJECT_SERVICE_EVENT_SET_ACTIVE_FAILED;
    }

    const char *action =
        deleting
        ? "Delete"
        : "Set active";

    if (
        state->event.result ==
        ESP_OK
    ) {
        ESP_LOGI(
            TAG,
            "%s completed: %s",
            action,
            command->object_id
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "%s failed for %s: %s",
            action,
            command->object_id,
            esp_err_to_name(
                state->event.result
            )
        );
    }

    if (
        state->event_callback !=
        NULL
    ) {
        state->event_callback(
            &state->event,
            state->event_user_context
        );
    }
}
