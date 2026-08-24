#include "storage_manager_internal.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"


static const char *TAG =
    "STORAGE_TEXT";


esp_err_t storage_manager_sidecar_path(
    const char *path,
    const char *suffix,
    char *result,
    size_t result_size
)
{
    if (
        path == NULL ||
        suffix == NULL ||
        result == NULL ||
        result_size == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int written =
        snprintf(
            result,
            result_size,
            "%s%s",
            path,
            suffix
        );

    if (
        written < 0 ||
        written >=
            (int)result_size
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}


esp_err_t storage_manager_recover_backup_locked(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat info;

    if (
        stat(
            path,
            &info
        ) == 0
    ) {
        return ESP_OK;
    }

    char backup_path[
        STORAGE_MANAGER_PATH_BUFFER_SIZE
    ];

    esp_err_t err =
        storage_manager_sidecar_path(
            path,
            ".bak",
            backup_path,
            sizeof(backup_path)
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Backup path is too long"
        );

        return err;
    }

    if (
        stat(
            backup_path,
            &info
        ) != 0
    ) {
        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "Recovering backup: %s",
        path
    );

    if (
        rename(
            backup_path,
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to recover backup for %s, errno=%d",
            path,
            errno
        );

        return ESP_FAIL;
    }

    return ESP_OK;
}
