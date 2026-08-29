#include "storage_manager_internal.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG =
    "STORAGE_TEXT";


static void storage_text_log_io(
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

    int64_t started_us =
        esp_timer_get_time();

    const int primary_stat_result =
        stat(
            path,
            &info
        );

    storage_text_log_io(
        "recover-primary-stat",
        path,
        primary_stat_result,
        started_us
    );

    if (primary_stat_result == 0) {
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

    started_us =
        esp_timer_get_time();

    const int backup_stat_result =
        stat(
            backup_path,
            &info
        );

    storage_text_log_io(
        "recover-backup-stat",
        backup_path,
        backup_stat_result,
        started_us
    );

    if (backup_stat_result != 0) {
        return ESP_OK;
    }

    ESP_LOGW(
        TAG,
        "Recovering backup: %s",
        path
    );

    started_us =
        esp_timer_get_time();

    const int rename_result =
        rename(
            backup_path,
            path
        );

    storage_text_log_io(
        "recover-backup-rename",
        path,
        rename_result,
        started_us
    );

    if (rename_result != 0) {
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
