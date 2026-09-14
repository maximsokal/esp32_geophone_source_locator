#include "storage_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_task_wdt.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif


static const char *TAG =
    "STORAGE_TREE";


#define STORAGE_TREE_PATH_SIZE          256
#define STORAGE_TREE_COOPERATE_INTERVAL   4


static esp_err_t storage_manager_tree_traversal_errno_to_error(
    int error_number
)
{
    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_FAIL;
}


static void storage_manager_tree_watchdog_checkpoint(void)
{
#ifdef ESP_PLATFORM
    /*
     * Only an explicitly subscribed owner task may feed TWDT. The ordinary
     * synchronous storage_manager callers remain completely unchanged.
     */
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        const esp_err_t err =
            esp_task_wdt_reset();

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "TWDT reset failed: %s",
                esp_err_to_name(err)
            );
        }
    }
#endif
}


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
         * Feed only after four completed filesystem mutations. A blocked
         * stat/readdir/remove/rmdir call cannot reach this checkpoint and is
         * therefore still visible to a subscribed Task Watchdog.
         */
        storage_manager_tree_watchdog_checkpoint();

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
    unsigned *operation_count,
    bool is_root
)
{
    struct stat info;

    if (
        stat(
            path,
            &info
        ) != 0
    ) {
        if (errno == ENOENT) {
            return is_root
                ? ESP_ERR_NOT_FOUND
                : ESP_FAIL;
        }

        return ESP_FAIL;
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

    struct dirent *entry = NULL;

    while (true) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                const int read_errno = errno;
                result =
                    storage_manager_tree_traversal_errno_to_error(
                        read_errno
                    );
                ESP_LOGE(
                    TAG,
                    "Failed to read directory: %s, errno=%d",
                    path,
                    read_errno
                );
            }
            break;
        }

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
                operation_count,
                false
            );

        if (result != ESP_OK) {
            break;
        }
    }

    errno = 0;
    const int close_result = closedir(directory);
    const int close_errno = errno;
    if (close_result != 0) {
        ESP_LOGE(
            TAG,
            "Failed to close directory: %s, errno=%d",
            path,
            close_errno
        );
        if (result == ESP_OK) {
            result = ESP_FAIL;
        }
    }

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
            &operation_count,
            true
        );

    storage_manager_operation_end();

    return err;
}
