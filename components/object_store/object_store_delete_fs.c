#include "object_store_internal.h"

#include "object_format.h"
#include "storage_manager.h"


#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"


esp_err_t object_store_fs_clear_active(void)
{
    if (
        !storage_manager_path_exists(
            ACTIVE_OBJECT_PATH
        )
    ) {
        return ESP_OK;
    }

    return
        storage_manager_remove_tree(
            ACTIVE_OBJECT_PATH
        );
}


esp_err_t object_store_fs_delete_object(
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

    return
        storage_manager_remove_tree(
            object_path
        );
}
