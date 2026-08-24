#include "object_store_internal.h"

#include <stdio.h>
#include <string.h>

#include "object_format.h"
#include "storage_manager.h"


#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"


esp_err_t object_store_fs_load_active(
    char *object_id,
    size_t object_id_size
)
{
    if (
        object_id == NULL ||
        object_id_size <
            OBJECT_STORE_ID_SIZE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    char buffer[64];

    esp_err_t err =
        storage_manager_read_text(
            ACTIVE_OBJECT_PATH,
            buffer,
            sizeof(buffer)
        );

    if (err != ESP_OK) {
        return err;
    }

    buffer[
        strcspn(
            buffer,
            "\r\n\t "
        )
    ] = '\0';

    if (
        !object_format_id_is_valid(
            buffer
        )
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    object_store_fs_copy_id(
        object_id,
        object_id_size,
        buffer
    );

    return ESP_OK;
}


esp_err_t object_store_fs_save_active(
    const char *object_id
)
{
    if (
        !object_format_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    char text[
        OBJECT_STORE_ID_SIZE + 2
    ];

    int written =
        snprintf(
            text,
            sizeof(text),
            "%s\n",
            object_id
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(text)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return
        storage_manager_atomic_write_text(
            ACTIVE_OBJECT_PATH,
            text
        );
}
