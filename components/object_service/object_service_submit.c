#include "object_service.h"

#include <string.h>

#include "object_service_internal.h"

#include "freertos/queue.h"


esp_err_t object_service_internal_submit(
    const object_service_command_t *command
)
{
    object_service_state_t *state =
        object_service_internal_state();

    if (
        !state->initialized ||
        state->command_queue == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state->busy) {
        return ESP_ERR_INVALID_STATE;
    }

    state->busy =
        true;

    if (
        xQueueSend(
            state->command_queue,
            command,
            0
        ) != pdTRUE
    ) {
        state->busy =
            false;

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


static esp_err_t object_service_submit_id(
    object_service_command_type_t type,
    const char *object_id
)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length =
        strlen(
            object_id
        );

    if (
        length == 0 ||
        length >=
            OBJECT_SERVICE_OBJECT_ID_SIZE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    object_service_command_t command = {
        .type =
            type,
    };

    memcpy(
        command.object_id,
        object_id,
        length + 1
    );

    return
        object_service_internal_submit(
            &command
        );
}


esp_err_t object_service_create_async(void)
{
    const object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_CREATE,
    };

    return
        object_service_internal_submit(
            &command
        );
}


esp_err_t object_service_list_async(void)
{
    const object_service_command_t command = {
        .type =
            OBJECT_SERVICE_COMMAND_LIST,
    };

    return
        object_service_internal_submit(
            &command
        );
}


esp_err_t object_service_set_active_async(
    const char *object_id
)
{
    return
        object_service_submit_id(
            OBJECT_SERVICE_COMMAND_SET_ACTIVE,
            object_id
        );
}


esp_err_t object_service_delete_async(
    const char *object_id
)
{
    return
        object_service_submit_id(
            OBJECT_SERVICE_COMMAND_DELETE,
            object_id
        );
}
