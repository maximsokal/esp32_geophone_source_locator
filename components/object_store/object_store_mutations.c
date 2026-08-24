#include "object_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "object_format.h"
#include "object_store_internal.h"


static const char *TAG =
    "OBJECT_STORE_OP";


esp_err_t object_store_set_active(
    const char *object_id
)
{
    if (
        !object_format_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        object_store_internal_begin_file_operation();

    if (err != ESP_OK) {
        return err;
    }

    if (
        !object_store_fs_object_exists(
            object_id
        )
    ) {
        object_store_internal_end_file_operation();

        return ESP_ERR_NOT_FOUND;
    }

    err =
        object_store_fs_save_active(
            object_id
        );

    if (err == ESP_OK) {
        object_store_internal_set_active_id(
            object_id
        );

        ESP_LOGI(
            TAG,
            "Active object: %s",
            object_id
        );
    }

    object_store_internal_end_file_operation();

    return err;
}


esp_err_t object_store_delete(
    const char *object_id
)
{
    if (
        !object_format_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        object_store_internal_begin_file_operation();

    if (err != ESP_OK) {
        return err;
    }

    if (
        !object_store_fs_object_exists(
            object_id
        )
    ) {
        object_store_internal_end_file_operation();

        return ESP_ERR_NOT_FOUND;
    }

    bool deleting_active =
        strcmp(
            object_store_internal_active_id(),
            object_id
        ) == 0;

    if (deleting_active) {
        err =
            object_store_fs_clear_active();

        if (err == ESP_OK) {
            object_store_internal_clear_active_id();
        }
    }

    if (err == ESP_OK) {
        err =
            object_store_fs_delete_object(
                object_id
            );
    }

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Deleted object: %s",
            object_id
        );
    }

    object_store_internal_end_file_operation();

    return err;
}
