#include "object_store.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"

#include "object_store_internal.h"


static const char *TAG =
    "OBJECT_STORE_OP";


static esp_err_t object_store_create_transaction(
    const char *initial_name,
    object_store_summary_t *created_object
)
{
    if (initial_name != NULL) {
        const esp_err_t validation_err =
            object_store_internal_validate_name(initial_name);
        if (validation_err != ESP_OK) {
            return validation_err;
        }
    }

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    char object_id[OBJECT_STORE_ID_SIZE] = {0};
    bool root_created = false;

    err = object_store_fs_next_id(object_id, sizeof(object_id));
    if (err == ESP_OK) {
        err = object_store_fs_create_layout(object_id, &root_created);
    }
    if (err == ESP_OK) {
        err = object_store_json_write_metadata(object_id, initial_name);
    }
    if (err == ESP_OK) {
        /* ACTIVE is the final persistent commit point. */
        err = object_store_fs_save_active(object_id);
    }

    if (err != ESP_OK) {
        const esp_err_t primary_err = err;

        if (root_created) {
            const esp_err_t rollback_err =
                object_store_fs_delete_object(object_id);
            if (rollback_err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Create %s failed (%s), rollback failed (%s)",
                    object_id[0] != '\0' ? object_id : "<unassigned>",
                    esp_err_to_name(primary_err),
                    esp_err_to_name(rollback_err)
                );
                object_store_profile_cache_reset();
            }
        }

        object_store_internal_end_file_operation();
        return primary_err;
    }

    const char *final_name =
        initial_name != NULL ? initial_name : object_id;

    /* Persistent commit succeeded. ACTIVE changes bounded membership when the
     * archive is full, so force the next profile snapshot through the same
     * canonical census instead of guessing an eviction in the RAM index. */
    object_store_internal_set_active_id(object_id);
    object_store_profile_cache_invalidate_membership_locked();

    if (created_object != NULL) {
        object_store_internal_fill_summary(created_object, object_id, true);
        snprintf(
            created_object->name,
            sizeof(created_object->name),
            "%s",
            final_name
        );
    }

    ESP_LOGI(TAG, "Created object: %s", object_id);

    object_store_internal_end_file_operation();
    return ESP_OK;
}


esp_err_t object_store_create(
    object_store_summary_t *created_object
)
{
    return object_store_create_transaction(NULL, created_object);
}


esp_err_t object_store_create_named(
    const char *initial_name,
    object_store_summary_t *created_object
)
{
    return object_store_create_transaction(initial_name, created_object);
}


esp_err_t object_store_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count
)
{
    if (objects == NULL || object_count == NULL || max_objects == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS
        ? max_objects
        : OBJECT_STORE_MAX_OBJECTS;
    object_store_profile_t *profiles = calloc(bounded, sizeof(*profiles));
    if (profiles == NULL) {
        *object_count = 0;
        return ESP_ERR_NO_MEM;
    }

    float defaults[OBJECT_STORE_POD_COUNT];
    size_t count = 0;

    /* Object-service LIST runs asynchronously, but it still shares SPI2 with
     * LVGL. Use the same cooperative profile walk as the Web-status cache so
     * a cold list cannot monopolize storage_manager for the whole archive. */
    esp_err_t err = object_store_list_profiles_responsive(
        profiles,
        bounded,
        &count,
        defaults
    );
    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            objects[i] = profiles[i].summary;
        }
        *object_count = count;
    }
    else {
        *object_count = 0;
    }

    free(profiles);
    return err;
}
