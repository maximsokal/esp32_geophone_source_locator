#include "storage_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

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

    bool write_ok =
        fputs(
            text,
            file
        ) >= 0;

    if (
        write_ok &&
        fflush(file) != 0
    ) {
        write_ok =
            false;
    }

    if (write_ok) {
        int descriptor =
            fileno(file);

        if (
            descriptor >= 0 &&
            fsync(descriptor) != 0
        ) {
            write_ok =
                false;
        }
    }

    if (
        fclose(file) != 0
    ) {
        write_ok =
            false;
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

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );

        return err;
    }

    char temp_path[
        STORAGE_MANAGER_PATH_BUFFER_SIZE
    ];

    char backup_path[
        STORAGE_MANAGER_PATH_BUFFER_SIZE
    ];

    err =
        storage_manager_sidecar_path(
            path,
            ".tmp",
            temp_path,
            sizeof(temp_path)
        );

    if (err == ESP_OK) {
        err =
            storage_manager_sidecar_path(
                path,
                ".bak",
                backup_path,
                sizeof(backup_path)
            );
    }

    if (err != ESP_OK) {
        storage_manager_operation_end();

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

    remove(
        temp_path
    );

    err =
        storage_manager_write_temp_file(
            temp_path,
            text
        );

    if (err != ESP_OK) {
        remove(
            temp_path
        );

        storage_manager_operation_end();

        return err;
    }

    struct stat info;

    bool had_original =
        stat(
            path,
            &info
        ) == 0;

    if (had_original) {
        remove(
            backup_path
        );

        if (
            rename(
                path,
                backup_path
            ) != 0
        ) {
            ESP_LOGE(
                TAG,
                "Failed to create backup for %s",
                path
            );

            remove(
                temp_path
            );

            storage_manager_operation_end();

            return ESP_FAIL;
        }
    }

    if (
        rename(
            temp_path,
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
                backup_path,
                path
            );
        }

        remove(
            temp_path
        );

        storage_manager_operation_end();

        return ESP_FAIL;
    }

    if (had_original) {
        remove(
            backup_path
        );
    }

    storage_manager_operation_end();

    return ESP_OK;
}
