#include "storage_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "STORAGE_TREE";


#define STORAGE_TREE_PATH_SIZE          256
#define STORAGE_TREE_COOPERATE_INTERVAL   4


static void storage_manager_tree_cooperate(
    unsigned *operation_count
)
{
    if (operation_count == NULL) {
        return;
    }

    (*operation_count)++;

    if (
        (
            *operation_count %
            STORAGE_TREE_COOPERATE_INTERVAL
        ) == 0
    ) {
        /*
         * Allow higher-priority monitoring and shutdown tasks
         * to run during a long recursive delete.
         *
         * The filesystem operation remains locked and is never
         * cancelled in the middle of a directory update.
         */
        vTaskDelay(
            1
        );
    }
}


static esp_err_t storage_manager_remove_tree_locked(
    const char *path,
    unsigned *operation_count
)
{
    struct stat info;

    if (
        stat(
            path,
            &info
        ) != 0
    ) {
        return
            errno == ENOENT
                ? ESP_ERR_NOT_FOUND
                : ESP_FAIL;
    }

    if (
        !S_ISDIR(
            info.st_mode
        )
    ) {
        if (
            remove(
                path
            ) != 0
        ) {
            ESP_LOGE(
                TAG,
                "Failed to remove file: %s, errno=%d",
                path,
                errno
            );

            return ESP_FAIL;
        }

        storage_manager_tree_cooperate(
            operation_count
        );

        return ESP_OK;
    }

    DIR *directory =
        opendir(
            path
        );

    if (directory == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to open directory: %s, errno=%d",
            path,
            errno
        );

        return ESP_FAIL;
    }

    esp_err_t result =
        ESP_OK;

    struct dirent *entry;

    while (
        (
            entry =
                readdir(
                    directory
                )
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
            STORAGE_TREE_PATH_SIZE
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
            storage_manager_remove_tree_locked(
                child_path,
                operation_count
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

    storage_manager_tree_cooperate(
        operation_count
    );

    return ESP_OK;
}


esp_err_t storage_manager_remove_tree(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        return err;
    }

    unsigned operation_count =
        0;

    err =
        storage_manager_remove_tree_locked(
            path,
            &operation_count
        );

    storage_manager_operation_end();

    return err;
}
