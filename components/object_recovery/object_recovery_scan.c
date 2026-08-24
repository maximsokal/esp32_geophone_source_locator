#include "object_recovery_internal.h"

#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "object_recovery_fs.h"
#include "storage_manager.h"


static const char *TAG =
    "OBJECT_RECOVERY";


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
)
{
    if (
        object_id == NULL ||
        removed == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *removed =
        false;

    char object_directory[
        OBJECT_RECOVERY_PATH_SIZE
    ];

    int written =
        snprintf(
            object_directory,
            sizeof(object_directory),
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );

    if (
        written < 0 ||
        written >=
            (int)sizeof(object_directory)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat info;

    if (
        stat(
            object_directory,
            &info
        ) != 0
    ) {
        return ESP_FAIL;
    }

    if (
        !S_ISDIR(
            info.st_mode
        )
    ) {
        return ESP_OK;
    }

    bool valid =
        false;

    bool recovered =
        false;

    esp_err_t err =
        object_recovery_fs_prepare_metadata(
            object_directory,
            object_id,
            &valid,
            &recovered
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Metadata recovery failed for %s",
            object_id
        );

        return err;
    }

    if (valid) {
        if (recovered) {
            ESP_LOGW(
                TAG,
                "Recovered metadata for %s",
                object_id
            );
        }

        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "Incomplete object found: %s",
        object_id
    );

    err =
        storage_manager_remove_tree(
            object_directory
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Removed incomplete object: %s",
        object_id
    );

    *removed =
        true;

    return ESP_OK;
}
