#include "object_store_internal.h"

#include "esp_log.h"


static const char *TAG =
    "OBJECT_STORE";


void object_store_internal_restore_active(void)
{
    char active_id[
        OBJECT_STORE_ID_SIZE
    ];

    esp_err_t err =
        object_store_fs_load_active(
            active_id,
            sizeof(active_id)
        );

    if (
        err == ESP_OK &&
        object_store_fs_object_exists(
            active_id
        )
    ) {
        object_store_internal_set_active_id(
            active_id
        );

        ESP_LOGI(
            TAG,
            "Restored active object: %s",
            active_id
        );

        return;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(
            TAG,
            "No active object"
        );

        return;
    }

    if (err == ESP_OK) {
        ESP_LOGW(
            TAG,
            "Active object does not exist: %s",
            active_id
        );

        return;
    }

    ESP_LOGW(
        TAG,
        "Active object marker is invalid"
    );
}
