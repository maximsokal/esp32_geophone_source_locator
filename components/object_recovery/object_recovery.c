#include "object_recovery.h"

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "object_recovery_fs.h"
#include "storage_manager.h"


static const char *TAG =
    "OBJECT_RECOVERY";


#define OBJECT_ID_LENGTH    8
#define RECOVERY_PATH_SIZE  256


static bool object_recovery_is_object_id(
    const char *name
)
{
    if (
        name ==
        NULL ||
        strlen(name) !=
            OBJECT_ID_LENGTH
    ) {
        return false;
    }


    if (
        strncmp(
            name,
            "OBJ_",
            4
        ) != 0
    ) {
        return false;
    }


    for (
        size_t i = 4;
        i < OBJECT_ID_LENGTH;
        ++i
    ) {
        if (
            name[i] < '0' ||
            name[i] > '9'
        ) {
            return false;
        }
    }


    return true;
}


static esp_err_t object_recovery_check_one(
    const char *object_id,
    bool *removed
)
{
    *removed =
        false;


    char object_directory[
        RECOVERY_PATH_SIZE
    ];


    const int written =
        snprintf(
            object_directory,
            sizeof(object_directory),
            "%s/%s",
            STORAGE_MANAGER_OBJECTS_PATH,
            object_id
        );


    if (
        written < 0 ||
        (size_t)written >=
            sizeof(object_directory)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    struct stat info;


    if (
        stat(
            object_directory,
            &info
        ) != 0
    ) {
        return ESP_FAIL;
    }


    if (
        !S_ISDIR(
            info.st_mode
        )
    ) {
        return ESP_OK;
    }


    bool valid =
        false;


    bool recovered =
        false;


    esp_err_t err =
        object_recovery_fs_prepare_metadata(
            object_directory,
            object_id,
            &valid,
            &recovered
        );


    if (
        err !=
        ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "Metadata recovery failed for %s",
            object_id
        );


        return err;
    }


    if (valid) {

        if (recovered) {
            ESP_LOGW(
                TAG,
                "Recovered metadata for %s",
                object_id
            );
        }


        return ESP_OK;
    }


    ESP_LOGW(
        TAG,
        "Incomplete object found: %s",
        object_id
    );


    err =
        object_recovery_fs_remove_tree(
            object_directory
        );


    if (
        err !=
        ESP_OK
    ) {
        ESP_LOGE(
            TAG,
            "Failed to remove incomplete object: %s",
            object_id
        );


        return err;
    }


    ESP_LOGI(
        TAG,
        "Removed incomplete object: %s",
        object_id
    );


    *removed =
        true;


    return ESP_OK;
}


esp_err_t object_recovery_run(void)
{
    ESP_LOGI(
        TAG,
        "Checking object storage"
    );


    /*
     * Restart directory enumeration whenever an incomplete
     * directory is removed. This avoids relying on directory
     * iteration behavior while entries are being deleted.
     */
    while (true) {

        DIR *directory =
            opendir(
                STORAGE_MANAGER_OBJECTS_PATH
            );


        if (
            directory ==
            NULL
        ) {
            ESP_LOGE(
                TAG,
                "Failed to open object directory"
            );


            return ESP_FAIL;
        }


        bool removed =
            false;


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
                !object_recovery_is_object_id(
                    entry->d_name
                )
            ) {
                continue;
            }


            result =
                object_recovery_check_one(
                    entry->d_name,
                    &removed
                );


            if (
                result !=
                    ESP_OK ||
                removed
            ) {
                break;
            }
        }


        closedir(
            directory
        );


        if (
            result !=
            ESP_OK
        ) {
            return result;
        }


        if (
            !removed
        ) {
            break;
        }
    }


    ESP_LOGI(
        TAG,
        "Object storage check complete"
    );


    return ESP_OK;
}