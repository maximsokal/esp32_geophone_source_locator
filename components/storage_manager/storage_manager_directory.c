#include "storage_manager.h"

#include <errno.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_DIR";


esp_err_t storage_manager_ensure_directory_locked(
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
        if (
            S_ISDIR(
                info.st_mode
            )
        ) {
            return ESP_OK;
        }

        ESP_LOGE(
            TAG,
            "Path exists but is not a directory: %s",
            path
        );

        return ESP_FAIL;
    }

    /*
     * FATFS VFS does not always report ENOENT for a
     * missing path. Try mkdir() regardless of stat errno.
     */
    errno =
        0;

    if (
        mkdir(
            path,
            0775
        ) == 0
    ) {
        ESP_LOGI(
            TAG,
            "Created directory: %s",
            path
        );

        return ESP_OK;
    }

    int mkdir_errno =
        errno;

    if (
        mkdir_errno ==
        EEXIST
    ) {
        if (
            stat(
                path,
                &info
            ) == 0 &&
            S_ISDIR(
                info.st_mode
            )
        ) {
            return ESP_OK;
        }

        ESP_LOGE(
            TAG,
            "Existing path is not a directory: %s",
            path
        );

        return ESP_FAIL;
    }

    ESP_LOGE(
        TAG,
        "mkdir failed for %s, errno=%d",
        path,
        mkdir_errno
    );

    return ESP_FAIL;
}


esp_err_t storage_manager_create_directory(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );

        return err;
    }

    err =
        storage_manager_ensure_directory_locked(
            path
        );

    storage_manager_operation_end();

    return err;
}


bool storage_manager_path_exists(
    const char *path
)
{
    if (path == NULL) {
        return false;
    }

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        return false;
    }

    struct stat info;

    bool exists =
        stat(
            path,
            &info
        ) == 0;

    storage_manager_operation_end();

    return exists;
}
