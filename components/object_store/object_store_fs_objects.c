#include "object_store_internal.h"

#include <stdio.h>

#include "object_format.h"
#include "storage_manager.h"


bool object_store_fs_object_exists(
    const char *object_id
)
{
    if (
        !object_format_id_is_valid(
            object_id
        )
    ) {
        return false;
    }

    char metadata_path[
        OBJECT_STORE_FS_PATH_SIZE
    ];

    if (
        object_store_fs_metadata_path(
            object_id,
            metadata_path,
            sizeof(metadata_path)
        ) != ESP_OK
    ) {
        return false;
    }

    return
        storage_manager_path_exists(
            metadata_path
        );
}


esp_err_t object_store_fs_create_layout(
    const char *object_id,
    bool *root_created
)
{
    if (root_created == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *root_created = false;

    if (
        !object_format_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    char object_path[
        OBJECT_STORE_FS_PATH_SIZE
    ];

    esp_err_t err =
        object_store_fs_object_path(
            object_id,
            object_path,
            sizeof(object_path)
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        storage_manager_create_directory_exclusive(
            object_path
        );

    if (err != ESP_OK) {
        return err;
    }

    *root_created = true;

    char sessions_path[
        OBJECT_STORE_FS_PATH_SIZE
    ];

    int written =
        snprintf(
            sessions_path,
            sizeof(sessions_path),
            "%s/sessions",
            object_path
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(sessions_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return
        storage_manager_create_directory(
            sessions_path
        );
}
