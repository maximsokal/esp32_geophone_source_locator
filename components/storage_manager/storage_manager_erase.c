#include "storage_manager.h"

#include "esp_log.h"


static const char *TAG =
    "STORAGE_ERASE";


esp_err_t storage_manager_erase_application_data(void)
{
    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(
        TAG,
        "Erasing application data: %s",
        STORAGE_MANAGER_ROOT_PATH
    );

    esp_err_t erase_err =
        storage_manager_remove_tree(
            STORAGE_MANAGER_ROOT_PATH
        );

    if (
        erase_err ==
        ESP_ERR_NOT_FOUND
    ) {
        erase_err =
            ESP_OK;
    }

    /*
     * Always restore the application directory layout.
     *
     * Even after a partial deletion failure, the manager stays
     * initialized and future boot recovery has valid roots.
     */
    esp_err_t restore_err =
        storage_manager_create_directory(
            STORAGE_MANAGER_ROOT_PATH
        );

    if (restore_err == ESP_OK) {
        restore_err =
            storage_manager_create_directory(
                STORAGE_MANAGER_OBJECTS_PATH
            );
    }

    storage_manager_operation_end();

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

    if (restore_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to restore storage layout: %s",
            esp_err_to_name(
                restore_err
            )
        );

        return restore_err;
    }

    ESP_LOGI(
        TAG,
        "Application data erased"
    );

    return ESP_OK;
}
