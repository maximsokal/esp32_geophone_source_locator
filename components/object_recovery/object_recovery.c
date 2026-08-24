#include "object_recovery.h"

#include <dirent.h>
#include <stdbool.h>

#include "esp_log.h"

#include "object_format.h"
#include "object_recovery_internal.h"
#include "storage_manager.h"


static const char *TAG =
    "OBJECT_RECOVERY";


esp_err_t object_recovery_run(void)
{
    esp_err_t guard_err =
        storage_manager_operation_begin();

    if (guard_err != ESP_OK) {
        return guard_err;
    }

    ESP_LOGI(
        TAG,
        "Checking object storage"
    );

    esp_err_t final_result =
        ESP_OK;

    while (true) {
        DIR *directory =
            opendir(
                STORAGE_MANAGER_OBJECTS_PATH
            );

        if (directory == NULL) {
            final_result =
                ESP_FAIL;

            ESP_LOGE(
                TAG,
                "Failed to open object directory"
            );

            break;
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
                !object_format_id_is_valid(
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
                result != ESP_OK ||
                removed
            ) {
                break;
            }
        }

        closedir(
            directory
        );

        if (result != ESP_OK) {
            final_result =
                result;

            break;
        }

        if (!removed) {
            break;
        }
    }

    storage_manager_operation_end();

    if (final_result == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Object storage check complete"
        );
    }

    return final_result;
}
