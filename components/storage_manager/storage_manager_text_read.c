#include "storage_manager.h"

#include <errno.h>
#include <stdio.h>

#include "esp_log.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_READ";


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
        storage_manager_recover_backup_locked(
            path
        );

    if (err != ESP_OK) {
        storage_manager_operation_end();

        return err;
    }

    FILE *file =
        fopen(
            path,
            "r"
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

    size_t count =
        fread(
            buffer,
            1,
            buffer_size - 1,
            file
        );

    if (
        ferror(file)
    ) {
        fclose(
            file
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
        int next_character =
            fgetc(
                file
            );

        if (
            next_character !=
            EOF
        ) {
            fclose(
                file
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

    fclose(
        file
    );

    storage_manager_operation_end();

    return ESP_OK;
}
