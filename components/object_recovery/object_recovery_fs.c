#include "object_recovery_fs.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>


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
    if (
        unlink(
            path
        ) == 0 ||
        errno == ENOENT
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

    return
        rename(
            source,
            destination
        ) == 0
        ? ESP_OK
        : ESP_FAIL;
}
