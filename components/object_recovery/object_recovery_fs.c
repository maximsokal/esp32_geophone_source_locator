#include "object_recovery_fs.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG =
    "OBJECT_RECOVERY_IO";


static void object_recovery_fs_log_io(
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


esp_err_t object_recovery_fs_build_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *file_name
)
{
    if (
        buffer == NULL ||
        directory == NULL ||
        file_name == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int written =
        snprintf(
            buffer,
            buffer_size,
            "%s/%s",
            directory,
            file_name
        );

    return
        written < 0 ||
        written >=
            (int)buffer_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}


esp_err_t object_recovery_fs_unlink_if_exists(
    const char *path
)
{
    errno =
        0;

    const int64_t started_us =
        esp_timer_get_time();

    const int unlink_result =
        unlink(
            path
        );

    const int unlink_errno =
        errno;

    object_recovery_fs_log_io(
        "unlink",
        path,
        unlink_result,
        started_us
    );

    if (
        unlink_result == 0 ||
        unlink_errno == ENOENT
    ) {
        return ESP_OK;
    }

    return ESP_FAIL;
}


esp_err_t object_recovery_fs_restore_file(
    const char *source,
    const char *destination
)
{
    esp_err_t err =
        object_recovery_fs_unlink_if_exists(
            destination
        );

    if (err != ESP_OK) {
        return err;
    }

    const int64_t started_us =
        esp_timer_get_time();

    const int rename_result =
        rename(
            source,
            destination
        );

    object_recovery_fs_log_io(
        "rename",
        destination,
        rename_result,
        started_us
    );

    return
        rename_result == 0
        ? ESP_OK
        : ESP_FAIL;
}
