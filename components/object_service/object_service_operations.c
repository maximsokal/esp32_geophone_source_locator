#include "object_service_internal.h"

#include <string.h>

#include "object_store.h"

#include "esp_log.h"


static const char *TAG =
    "OBJECT_SERVICE";


static void object_service_notify(
    object_service_state_t *state
)
{
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


static void object_service_prepare_event(
    object_service_state_t *state
)
{
    memset(
        &state->event,
        0,
        sizeof(state->event)
    );

    state->event.result =
        ESP_OK;
}


static void object_service_process_create(
    object_service_state_t *state
)
{
    object_service_prepare_event(
        state
    );

    state->event.result =
        object_store_create(
            &state->event.object
        );

    state->event.type =
        state->event.result == ESP_OK
        ? OBJECT_SERVICE_EVENT_CREATE_DONE
        : OBJECT_SERVICE_EVENT_CREATE_FAILED;

    if (
        state->event.result ==
        ESP_OK
    ) {
        ESP_LOGI(
            TAG,
            "Create completed: %s",
            state->event.object.id
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "Create failed: %s",
            esp_err_to_name(
                state->event.result
            )
        );
    }

    object_service_notify(
        state
    );
}


static void object_service_process_list(
    object_service_state_t *state
)
{
    object_service_prepare_event(
        state
    );

    state->event.result =
        object_store_list(
            state->event.objects,
            OBJECT_SERVICE_MAX_OBJECTS,
            &state->event.object_count
        );

    state->event.type =
        state->event.result == ESP_OK
        ? OBJECT_SERVICE_EVENT_LIST_DONE
        : OBJECT_SERVICE_EVENT_LIST_FAILED;

    if (
        state->event.result ==
        ESP_OK
    ) {
        ESP_LOGI(
            TAG,
            "List completed: %u object(s)",
            (unsigned)
                state->event.object_count
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "List failed: %s",
            esp_err_to_name(
                state->event.result
            )
        );
    }

    object_service_notify(
        state
    );
}


void object_service_internal_process(
    const object_service_command_t *command
)
{
    object_service_state_t *state =
        object_service_internal_state();

    switch (command->type) {
        case OBJECT_SERVICE_COMMAND_CREATE:
            object_service_process_create(
                state
            );
            break;

        case OBJECT_SERVICE_COMMAND_LIST:
            object_service_process_list(
                state
            );
            break;

        case OBJECT_SERVICE_COMMAND_SET_ACTIVE:
        case OBJECT_SERVICE_COMMAND_DELETE:
            object_service_internal_process_mutation(
                command
            );
            break;

        case OBJECT_SERVICE_COMMAND_STOP:
        default:
            break;
    }
}
