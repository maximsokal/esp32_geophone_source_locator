#include "object_store_internal.h"

#include <dirent.h>
#include <stdio.h>

#include "object_format.h"
#include "storage_manager.h"


esp_err_t object_store_fs_next_id(
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

    DIR *directory =
        opendir(
            STORAGE_MANAGER_OBJECTS_PATH
        );

    if (directory == NULL) {
        return ESP_FAIL;
    }

    unsigned max_number =
        0;

    struct dirent *entry;

    while (
        (
            entry =
                readdir(
                    directory
                )
        ) != NULL
    ) {
        if (
            !object_format_id_is_valid(
                entry->d_name
            )
        ) {
            continue;
        }

        unsigned number =
            0;

        if (
            sscanf(
                entry->d_name,
                "OBJ_%4u",
                &number
            ) == 1 &&
            number >
                max_number
        ) {
            max_number =
                number;
        }
    }

    closedir(
        directory
    );

    if (max_number >= 9999) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written =
        snprintf(
            object_id,
            object_id_size,
            "OBJ_%04u",
            max_number + 1
        );

    return
        written < 0 ||
        written >=
            (int)object_id_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}
