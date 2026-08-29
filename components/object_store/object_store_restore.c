#include "object_store_internal.h"

#include <stdint.h>

#include "esp_log.h"


static const char *TAG =
    "OBJECT_STORE";


static void object_store_log_boot_timing(
    const char *stage,
    uint32_t started_ms
)
{
    ESP_LOGI(
        TAG,
        "BOOT_TIMING: %s=%u ms",
        stage,
        (unsigned)(
            esp_log_timestamp() -
            started_ms
        )
    );
}


void object_store_internal_restore_active(void)
{
    char active_id[
        OBJECT_STORE_ID_SIZE
    ];

    uint32_t stage_started_ms =
        esp_log_timestamp();

    esp_err_t err =
        object_store_fs_load_active(
            active_id,
            sizeof(active_id)
        );

    object_store_log_boot_timing(
        "load_active_object_marker",
        stage_started_ms
    );

    if (err == ESP_OK) {
        stage_started_ms =
            esp_log_timestamp();

        const bool object_exists =
            object_store_fs_object_exists(
                active_id
            );

        object_store_log_boot_timing(
            "check_active_object_exists",
            stage_started_ms
        );

        if (object_exists) {
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

        ESP_LOGW(
            TAG,
            "Active object does not exist: %s",
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

    ESP_LOGW(
        TAG,
        "Active object marker is invalid"
    );
}
