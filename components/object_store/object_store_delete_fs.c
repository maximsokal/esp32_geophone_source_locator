#include "object_store_internal.h"

#include <stdio.h>

#include "storage_manager.h"


#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"

#define OBJECT_DELETE_PATH_SIZE  256


esp_err_t object_store_fs_clear_active(void)
{
    if (
        !storage_manager_path_exists(
            ACTIVE_OBJECT_PATH
        )
    ) {
        return ESP_OK;
    }

    return storage_manager_remove_tree(
        ACTIVE_OBJECT_PATH
    );
}


esp_err_t object_store_fs_delete_object(
    const char *object_id
)
{
    if (
        !object_store_fs_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    char object_path[
        OBJECT_DELETE_PATH_SIZE
    ];

    int written =
        snprintf(
            object_path,
            sizeof(object_path),
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(object_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return storage_manager_remove_tree(
        object_path
    );
}
