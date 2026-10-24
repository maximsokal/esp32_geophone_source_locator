#include "object_recovery_fs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _MSC_VER
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

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
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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


static int object_recovery_fs_rename_logged(
    const char *operation,
    const char *source,
    const char *destination
)
{
    const int64_t started_us =
        esp_timer_get_time();

    const int rename_result =
        OBJECT_RECOVERY_RENAME(
            source,
            destination
        );

    object_recovery_fs_log_io(
        operation,
        destination,
        rename_result,
        started_us
    );
    (void)source;

    return rename_result;
}


esp_err_t object_recovery_fs_restore_file(
    const char *source,
    const char *destination
)
{
    if (source == NULL || destination == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * FATFS (vfs_fat_rename -> f_rename) refuses to rename over an existing
     * file, so the destination has to move out of the way first. Never unlink
     * it: if the second rename fails, the original must still be recoverable.
     */
    char aside[OBJECT_RECOVERY_PATH_SIZE + 8];
    const int aside_length =
        snprintf(
            aside,
            sizeof(aside),
            "%s.old",
            destination
        );
    if (
        aside_length < 0 ||
        (size_t)aside_length >= sizeof(aside)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat info;
    errno = 0;
    const bool destination_exists =
        stat(
            destination,
            &info
        ) == 0;
    if (!destination_exists && errno != ENOENT) {
        /* Cannot tell whether a file is there: change nothing. */
        return ESP_FAIL;
    }

    if (destination_exists) {
        /* A leftover from an interrupted earlier restore is stale by now. */
        esp_err_t err =
            object_recovery_fs_unlink_if_exists(
                aside
            );

        if (err != ESP_OK) {
            return err;
        }

        if (
            object_recovery_fs_rename_logged(
                "rename-aside",
                destination,
                aside
            ) != 0
        ) {
            return ESP_FAIL;
        }
    }

    if (
        object_recovery_fs_rename_logged(
            "rename",
            source,
            destination
        ) != 0
    ) {
        if (destination_exists) {
            (void)object_recovery_fs_rename_logged(
                "rename-rollback",
                aside,
                destination
            );
        }

        return ESP_FAIL;
    }

    if (destination_exists) {
        /* Best effort; a left-over .old is harmless and cleaned next time. */
        (void)object_recovery_fs_unlink_if_exists(
            aside
        );
    }

    return ESP_OK;
}
