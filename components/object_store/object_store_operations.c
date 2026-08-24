#include "object_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "object_store_internal.h"


static const char *TAG =
    "OBJECT_STORE_OP";


esp_err_t object_store_create(
    object_store_summary_t *created_object
)
{
    esp_err_t err =
        object_store_internal_begin_file_operation();

    if (err != ESP_OK) {
        return err;
    }

    char object_id[
        OBJECT_STORE_ID_SIZE
    ];

    err =
        object_store_fs_next_id(
            object_id,
            sizeof(object_id)
        );

    if (err == ESP_OK) {
        err =
            object_store_fs_create_layout(
                object_id
            );
    }

    if (err == ESP_OK) {
        err =
            object_store_json_write_metadata(
                object_id
            );
    }

    if (err == ESP_OK) {
        err =
            object_store_fs_save_active(
                object_id
            );
    }

    if (err == ESP_OK) {
        object_store_internal_set_active_id(
            object_id
        );

        if (created_object != NULL) {
            object_store_internal_fill_summary(
                created_object,
                object_id,
                true
            );
        }

        ESP_LOGI(
            TAG,
            "Created object: %s",
            object_id
        );
    }

    object_store_internal_end_file_operation();

    return err;
}


esp_err_t object_store_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count
)
{
    if (
        objects == NULL ||
        object_count == NULL ||
        max_objects == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        object_store_internal_begin_file_operation();

    if (err != ESP_OK) {
        return err;
    }

    err =
        object_store_fs_list(
            objects,
            max_objects,
            object_count,
            object_store_internal_active_id()
        );

    object_store_internal_end_file_operation();

    return err;
}


esp_err_t object_store_set_active(
    const char *object_id
)
{
    if (
        !object_store_fs_id_is_valid(
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
        !object_store_fs_id_is_valid(
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

    /*
     * Clear the active marker first.
     *
     * If deletion is interrupted after this point,
     * the remaining object is simply inactive.
     */
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
