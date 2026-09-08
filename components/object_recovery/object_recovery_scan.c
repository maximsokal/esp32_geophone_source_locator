#include "object_recovery_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "object_format.h"
#include "object_recovery_fs.h"
#include "storage_manager.h"


#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif


static const char *TAG =
    "OBJECT_RECOVERY";


static void object_recovery_log_boot_timing(
    const char *object_id,
    const char *stage,
    uint32_t started_ms
)
{
    ESP_LOGI(
        TAG,
        "BOOT_TIMING: object=%s %s=%u ms",
        object_id,
        stage,
        (unsigned)(
            esp_log_timestamp() -
            started_ms
        )
    );
}


esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
)
{
    if (
        object_id == NULL ||
        !object_format_id_is_valid(object_id) ||
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

    uint32_t stage_started_ms =
        esp_log_timestamp();

    const int stat_result =
        stat(
            object_directory,
            &info
        );

    object_recovery_log_boot_timing(
        object_id,
        "directory_stat",
        stage_started_ms
    );

    if (stat_result != 0) {
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

    stage_started_ms =
        esp_log_timestamp();

    esp_err_t err =
        object_recovery_fs_prepare_metadata(
            object_directory,
            object_id,
            &valid,
            &recovered
        );

    object_recovery_log_boot_timing(
        object_id,
        "prepare_metadata",
        stage_started_ms
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

    stage_started_ms =
        esp_log_timestamp();

    err =
        storage_manager_remove_tree(
            object_directory
        );

    object_recovery_log_boot_timing(
        object_id,
        "remove_incomplete_tree",
        stage_started_ms
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
