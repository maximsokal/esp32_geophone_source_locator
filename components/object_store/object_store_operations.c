#include "object_store.h"

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

        /* The committed metadata is known; do not force a full SD rescan. */
        object_store_profile_cache_note_create_locked(object_id);
        object_store_profile_cache_note_active_locked(object_id);

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

    object_store_profile_t profiles[OBJECT_STORE_MAX_OBJECTS];
    size_t count = 0;
    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS
        ? max_objects
        : OBJECT_STORE_MAX_OBJECTS;

    esp_err_t err = object_store_list_profiles(profiles, bounded, &count);
    if (err != ESP_OK) {
        *object_count = 0;
        return err;
    }

    for (size_t i = 0; i < count; ++i) {
        objects[i] = profiles[i].summary;
    }
    *object_count = count;
    return ESP_OK;
}
