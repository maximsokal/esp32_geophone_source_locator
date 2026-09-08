#include "storage_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_WRITE";


static esp_err_t storage_manager_write_temp_file(
    const char *temp_path,
    const char *text
)
{
    FILE *file =
        fopen(
            temp_path,
            "w"
        );

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to open temporary file: %s",
            temp_path
        );

        return ESP_FAIL;
    }

    const int64_t fputs_started_us =
        esp_timer_get_time();
    const int fputs_result = fputs(text, file);
    ESP_LOGI(
        TAG,
        "IO_TIMING: temp-fputs path=%s result=%d elapsed=%lld us",
        temp_path,
        fputs_result,
        (long long)(esp_timer_get_time() - fputs_started_us)
    );
    bool write_ok = fputs_result >= 0;

    if (write_ok) {
        const int64_t fflush_started_us =
            esp_timer_get_time();
        const int fflush_result = fflush(file);
        ESP_LOGI(
            TAG,
            "IO_TIMING: temp-fflush path=%s result=%d elapsed=%lld us",
            temp_path,
            fflush_result,
            (long long)(esp_timer_get_time() - fflush_started_us)
        );
        if (fflush_result != 0) {
            write_ok = false;
        }
    }

    if (write_ok) {
        int descriptor =
            fileno(file);

        const int64_t fsync_started_us =
            esp_timer_get_time();
        const bool fsync_attempted = descriptor >= 0;
        const int fsync_result =
            fsync_attempted
                ? fsync(descriptor)
                : 0;
        ESP_LOGI(
            TAG,
            "IO_TIMING: temp-fsync path=%s result=%d elapsed=%lld us",
            temp_path,
            fsync_result,
            (long long)(esp_timer_get_time() - fsync_started_us)
        );
        if (fsync_attempted && fsync_result != 0) {
            write_ok = false;
        }
    }

    const int64_t fclose_started_us =
        esp_timer_get_time();
    const int fclose_result = fclose(file);
    ESP_LOGI(
        TAG,
        "IO_TIMING: temp-fclose path=%s result=%d elapsed=%lld us",
        temp_path,
        fclose_result,
        (long long)(esp_timer_get_time() - fclose_started_us)
    );
    if (fclose_result != 0) {
        write_ok = false;
    }

    if (!write_ok) {
        ESP_LOGE(
            TAG,
            "Failed to write temporary file: %s",
            temp_path
        );

        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t storage_manager_atomic_write_text(
    const char *path,
    const char *text
)
{
    if (
        path == NULL ||
        text == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    storage_manager_atomic_paths_t *paths =
        malloc(sizeof(*paths));
    if (paths == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );

        free(paths);
        return err;
    }

    err =
        storage_manager_sidecar_path(
            path,
            ".tmp",
            paths->temp_path,
            sizeof(paths->temp_path)
        );

    if (err == ESP_OK) {
        err =
            storage_manager_sidecar_path(
                path,
                ".bak",
                paths->backup_path,
                sizeof(paths->backup_path)
            );
    }

    if (err != ESP_OK) {
        goto done;
    }

    err =
        storage_manager_recover_backup_locked(
            path
        );

    if (err != ESP_OK) {
        goto done;
    }

    remove(
        paths->temp_path
    );

    err =
        storage_manager_write_temp_file(
            paths->temp_path,
            text
        );

    if (err != ESP_OK) {
        remove(
            paths->temp_path
        );
        goto done;
    }

    struct stat info;

    bool had_original =
        stat(
            path,
            &info
        ) == 0;

    if (had_original) {
        remove(
            paths->backup_path
        );

        if (
            rename(
                path,
                paths->backup_path
            ) != 0
        ) {
            ESP_LOGE(
                TAG,
                "Failed to create backup for %s",
                path
            );

            remove(
                paths->temp_path
            );
            err = ESP_FAIL;
            goto done;
        }
    }

    if (
        rename(
            paths->temp_path,
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to replace file: %s",
            path
        );

        if (had_original) {
            rename(
                paths->backup_path,
                path
            );
        }

        remove(
            paths->temp_path
        );
        err = ESP_FAIL;
        goto done;
    }

    if (had_original) {
        remove(
            paths->backup_path
        );
    }

done:
    storage_manager_operation_end();
    free(paths);
    return err;
}
