#include "storage_manager.h"

#include <errno.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_DIR";


static void storage_directory_log_io(
    const char *operation,
    const char *path,
    int result,
    int64_t started_us
)
{
    ESP_LOGI(
        TAG,
        "IO_TIMING: %s path=%s result=%d elapsed=%lld us",
        operation,
        path,
        result,
        (long long)(
            esp_timer_get_time() -
            started_us
        )
    );
}


esp_err_t storage_manager_ensure_directory_locked(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat info;

    int64_t started_us =
        esp_timer_get_time();

    const int stat_result =
        stat(
            path,
            &info
        );

    storage_directory_log_io(
        "stat",
        path,
        stat_result,
        started_us
    );

    if (stat_result == 0) {
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

    started_us =
        esp_timer_get_time();

    const int mkdir_result =
        mkdir(
            path,
            0775
        );

    storage_directory_log_io(
        "mkdir",
        path,
        mkdir_result,
        started_us
    );

    if (mkdir_result == 0) {
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
        started_us =
            esp_timer_get_time();

        const int recheck_result =
            stat(
                path,
                &info
            );

        storage_directory_log_io(
            "stat-after-EEXIST",
            path,
            recheck_result,
            started_us
        );

        if (
            recheck_result == 0 &&
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

    int64_t started_us =
        esp_timer_get_time();

    esp_err_t err =
        storage_manager_operation_begin();

    storage_directory_log_io(
        "lock-wait",
        path,
        (int)err,
        started_us
    );

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

    int64_t started_us =
        esp_timer_get_time();

    esp_err_t err =
        storage_manager_operation_begin();

    storage_directory_log_io(
        "path-exists-lock-wait",
        path,
        (int)err,
        started_us
    );

    if (err != ESP_OK) {
        return false;
    }

    struct stat info;

    started_us =
        esp_timer_get_time();

    const int stat_result =
        stat(
            path,
            &info
        );

    storage_directory_log_io(
        "path-exists-stat",
        path,
        stat_result,
        started_us
    );

    const bool exists =
        stat_result == 0;

    storage_manager_operation_end();

    return exists;
}
