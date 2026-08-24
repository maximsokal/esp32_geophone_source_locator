#include "storage_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"


static const char *TAG =
    "STORAGE_ERASE";


#define ERASE_PATH_SIZE 256


static esp_err_t storage_erase_tree(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat info;

    if (
        stat(
            path,
            &info
        ) != 0
    ) {
        return
            errno == ENOENT
                ? ESP_OK
                : ESP_FAIL;
    }

    if (!S_ISDIR(info.st_mode)) {
        return
            remove(path) == 0
                ? ESP_OK
                : ESP_FAIL;
    }

    DIR *directory =
        opendir(
            path
        );

    if (directory == NULL) {
        return ESP_FAIL;
    }

    esp_err_t result =
        ESP_OK;

    struct dirent *entry;

    while (
        (
            entry =
                readdir(directory)
        ) != NULL
    ) {
        if (
            strcmp(
                entry->d_name,
                "."
            ) == 0 ||
            strcmp(
                entry->d_name,
                ".."
            ) == 0
        ) {
            continue;
        }

        char child_path[
            ERASE_PATH_SIZE
        ];

        int written =
            snprintf(
                child_path,
                sizeof(child_path),
                "%s/%s",
                path,
                entry->d_name
            );

        if (
            written < 0 ||
            written >=
                (int)sizeof(child_path)
        ) {
            result =
                ESP_ERR_INVALID_SIZE;

            break;
        }

        result =
            storage_erase_tree(
                child_path
            );

        if (result != ESP_OK) {
            break;
        }
    }

    closedir(
        directory
    );

    if (result != ESP_OK) {
        return result;
    }

    if (
        rmdir(
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed to remove directory: %s, errno=%d",
            path,
            errno
        );

        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t storage_manager_erase_application_data(void)
{
    if (
        !storage_manager_is_ready()
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(
        TAG,
        "Erasing application data: %s",
        STORAGE_MANAGER_ROOT_PATH
    );

    /*
     * Wait for any storage-manager operation already in
     * progress and reject new operations while deleting.
     */
    esp_err_t shutdown_err =
        storage_manager_shutdown();

    if (shutdown_err != ESP_OK) {
        return shutdown_err;
    }

    esp_err_t erase_err =
        storage_erase_tree(
            STORAGE_MANAGER_ROOT_PATH
        );

    /*
     * Always try to restore the application layout.
     *
     * Even after a partial deletion failure, the firmware
     * must not leave the storage manager permanently stopped.
     */
    esp_err_t init_err =
        storage_manager_init();

    if (erase_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Application data erase failed: %s",
            esp_err_to_name(
                erase_err
            )
        );

        return erase_err;
    }

    if (init_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to restore storage layout: %s",
            esp_err_to_name(
                init_err
            )
        );

        return init_err;
    }

    ESP_LOGI(
        TAG,
        "Application data erased"
    );

    return ESP_OK;
}