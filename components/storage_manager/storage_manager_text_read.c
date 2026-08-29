#include "storage_manager.h"

#include <errno.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_READ";


static void storage_read_log_io(
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


esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    if (
        path == NULL ||
        buffer == NULL ||
        buffer_size < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t started_us =
        esp_timer_get_time();

    esp_err_t err =
        storage_manager_operation_begin();

    storage_read_log_io(
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

    started_us =
        esp_timer_get_time();

    err =
        storage_manager_recover_backup_locked(
            path
        );

    storage_read_log_io(
        "recover-backup",
        path,
        (int)err,
        started_us
    );

    if (err != ESP_OK) {
        storage_manager_operation_end();

        return err;
    }

    started_us =
        esp_timer_get_time();

    FILE *file =
        fopen(
            path,
            "r"
        );

    storage_read_log_io(
        "fopen",
        path,
        file == NULL ? -1 : 0,
        started_us
    );

    if (file == NULL) {
        int open_errno =
            errno;

        storage_manager_operation_end();

        if (
            open_errno ==
            ENOENT
        ) {
            return ESP_ERR_NOT_FOUND;
        }

        return ESP_ERR_NOT_FOUND;
    }

    started_us =
        esp_timer_get_time();

    size_t count =
        fread(
            buffer,
            1,
            buffer_size - 1,
            file
        );

    storage_read_log_io(
        "fread",
        path,
        ferror(file) ? -1 : (int)count,
        started_us
    );

    if (
        ferror(file)
    ) {
        started_us =
            esp_timer_get_time();

        const int close_result =
            fclose(
                file
            );

        storage_read_log_io(
            "fclose-after-read-error",
            path,
            close_result,
            started_us
        );

        storage_manager_operation_end();

        return ESP_FAIL;
    }

    buffer[count] =
        '\0';

    if (
        count ==
        buffer_size - 1
    ) {
        started_us =
            esp_timer_get_time();

        int next_character =
            fgetc(
                file
            );

        storage_read_log_io(
            "fgetc-size-check",
            path,
            next_character,
            started_us
        );

        if (
            next_character !=
            EOF
        ) {
            started_us =
                esp_timer_get_time();

            const int close_result =
                fclose(
                    file
                );

            storage_read_log_io(
                "fclose-after-size-error",
                path,
                close_result,
                started_us
            );

            storage_manager_operation_end();

            ESP_LOGE(
                TAG,
                "Text file is larger than buffer: %s",
                path
            );

            return ESP_ERR_INVALID_SIZE;
        }
    }

    started_us =
        esp_timer_get_time();

    const int close_result =
        fclose(
            file
        );

    storage_read_log_io(
        "fclose",
        path,
        close_result,
        started_us
    );

    storage_manager_operation_end();

    /*
     * Preserve the original read semantics: close timing is
     * diagnostic only for this bring-up investigation.
     */
    return ESP_OK;
}
