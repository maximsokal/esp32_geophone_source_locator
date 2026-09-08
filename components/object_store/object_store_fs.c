#include "object_store_internal.h"

#include <stdio.h>
#include <string.h>

#include "object_format.h"
#include "storage_manager.h"


esp_err_t object_store_fs_object_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    if (
        object_id == NULL ||
        path == NULL ||
        !object_format_id_is_valid(object_id)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int written =
        snprintf(
            path,
            path_size,
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );

    return
        written < 0 ||
        written >=
            (int)path_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}


esp_err_t object_store_fs_metadata_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    if (
        object_id == NULL ||
        path == NULL ||
        !object_format_id_is_valid(object_id)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int written =
        snprintf(
            path,
            path_size,
            "%s/%s/object.json",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );

    return
        written < 0 ||
        written >=
            (int)path_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}


void object_store_fs_copy_id(
    char *destination,
    size_t destination_size,
    const char *object_id
)
{
    if (
        destination == NULL ||
        object_id == NULL ||
        destination_size <=
            OBJECT_FORMAT_ID_TEXT_LENGTH
    ) {
        return;
    }

    memcpy(
        destination,
        object_id,
        OBJECT_FORMAT_ID_TEXT_LENGTH
    );

    destination[
        OBJECT_FORMAT_ID_TEXT_LENGTH
    ] = '\0';
}
