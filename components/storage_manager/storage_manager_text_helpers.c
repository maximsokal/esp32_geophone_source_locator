#include "storage_manager_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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


static esp_err_t storage_text_errno_to_error(
    int error_number
)
{
    if (error_number == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_FAIL;
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

    const int primary_stat_errno =
        primary_stat_result == 0
            ? 0
            : errno;

    storage_text_log_io(
        "recover-primary-stat",
        path,
        primary_stat_result,
        started_us
    );

    if (primary_stat_result == 0) {
        return ESP_OK;
    }

    /*
     * Only a genuinely missing primary file is a reason to inspect .bak.
     * FAT/SDSPI I/O failures used to be treated as ENOENT here, causing two
     * additional sector reads and eventually a false ESP_ERR_NOT_FOUND in the
     * web object database.
     */
    if (primary_stat_errno != ENOENT) {
        ESP_LOGE(
            TAG,
            "Primary stat failed for %s: errno=%d",
            path,
            primary_stat_errno
        );

        return storage_text_errno_to_error(
            primary_stat_errno
        );
    }

    char *backup_path = malloc(STORAGE_MANAGER_PATH_BUFFER_SIZE);
    if (backup_path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        storage_manager_sidecar_path(
            path,
            ".bak",
            backup_path,
            STORAGE_MANAGER_PATH_BUFFER_SIZE
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Backup path is too long"
        );

        free(backup_path);
        return err;
    }

    started_us =
        esp_timer_get_time();

    const int backup_stat_result =
        stat(
            backup_path,
            &info
        );

    const int backup_stat_errno =
        backup_stat_result == 0
            ? 0
            : errno;

    storage_text_log_io(
        "recover-backup-stat",
        backup_path,
        backup_stat_result,
        started_us
    );

    if (backup_stat_result != 0) {
        if (backup_stat_errno == ENOENT) {
            free(backup_path);
            return ESP_OK;
        }

        ESP_LOGE(
            TAG,
            "Backup stat failed for %s: errno=%d",
            backup_path,
            backup_stat_errno
        );

        err = storage_text_errno_to_error(
            backup_stat_errno
        );
        free(backup_path);
        return err;
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

    const int rename_errno =
        rename_result == 0
            ? 0
            : errno;

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
            rename_errno
        );

        err = storage_text_errno_to_error(
            rename_errno
        );
        free(backup_path);
        return err;
    }

    free(backup_path);
    return ESP_OK;
}
