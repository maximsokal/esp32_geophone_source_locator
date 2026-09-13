#include "object_recovery.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "object_format.h"
#include "object_recovery_internal.h"
#include "storage_manager.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


static const char *TAG =
    "OBJECT_RECOVERY";

#define ACTIVE_OBJECT_PATH \
    STORAGE_MANAGER_ROOT_PATH "/active_object.txt"

typedef char object_recovery_id_t[OBJECT_FORMAT_ID_SIZE];


static int object_recovery_compare_ids(
    const void *left,
    const void *right
)
{
    return strcmp(
        (const char *)left,
        (const char *)right
    );
}


static esp_err_t object_recovery_collect_ids(
    object_recovery_id_t **ids_out,
    size_t *count_out
)
{
    if (ids_out == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *ids_out = NULL;
    *count_out = 0U;

    esp_err_t err =
        storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory =
        opendir(
            STORAGE_MANAGER_OBJECTS_PATH
        );

    if (directory == NULL) {
        storage_manager_operation_end();
        ESP_LOGE(TAG, "Failed to open object directory");
        return ESP_FAIL;
    }

    object_recovery_id_t *ids = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    errno = 0;

    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (!object_format_id_is_valid(entry->d_name)) {
            continue;
        }

        if (count == capacity) {
            const size_t next_capacity =
                capacity == 0U ? 16U : capacity * 2U;
            object_recovery_id_t *grown = realloc(
                ids,
                next_capacity * sizeof(*ids)
            );
            if (grown == NULL) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            ids = grown;
            capacity = next_capacity;
        }

        snprintf(
            ids[count],
            sizeof(ids[count]),
            "%s",
            entry->d_name
        );
        ++count;
    }

    const int read_errno = errno;
    const int close_result = closedir(directory);
    storage_manager_operation_end();

    if (err == ESP_OK && read_errno != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && close_result != 0) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        free(ids);
        return err;
    }

    if (count > 1U) {
        qsort(
            ids,
            count,
            sizeof(*ids),
            object_recovery_compare_ids
        );
    }

    *ids_out = ids;
    *count_out = count;
    return ESP_OK;
}


static bool object_recovery_should_stop(
    object_recovery_stop_requested_fn stop_requested,
    void *user_context
)
{
    return
        stop_requested != NULL &&
        stop_requested(user_context);
}


static esp_err_t object_recovery_check_guarded(
    const char *object_id,
    bool *removed
)
{
    esp_err_t err =
        storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    err =
        object_recovery_check_one(
            object_id,
            removed
        );

    storage_manager_operation_end();
    return err;
}


esp_err_t object_recovery_run_cooperative(
    object_recovery_stop_requested_fn stop_requested,
    void *user_context,
    bool yield_between_objects
)
{
    ESP_LOGI(TAG, "Checking object storage mode=cooperative");

    const int64_t started_us = esp_timer_get_time();
    object_recovery_id_t *ids = NULL;
    size_t count = 0U;

    esp_err_t err =
        object_recovery_collect_ids(
            &ids,
            &count
        );
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_CENSUS objects=%u elapsed_ms=%lld",
        (unsigned)count,
        (long long)((esp_timer_get_time() - started_us) / 1000LL)
    );

    size_t processed = 0U;
    size_t removed_count = 0U;
    size_t recovered_or_valid = 0U;

    for (size_t index = 0U; index < count; ++index) {
        if (object_recovery_should_stop(stop_requested, user_context)) {
            ESP_LOGI(
                TAG,
                "OBJECT_RECOVERY_STOP processed=%u total=%u",
                (unsigned)processed,
                (unsigned)count
            );
            break;
        }

        const int64_t object_started_us = esp_timer_get_time();
        bool removed = false;
        err = object_recovery_check_guarded(
            ids[index],
            &removed
        );
        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Recovery failed object=%s err=%s",
                ids[index],
                esp_err_to_name(err)
            );
            break;
        }

        ++processed;
        if (removed) {
            ++removed_count;
        }
        else {
            ++recovered_or_valid;
        }

        ESP_LOGI(
            TAG,
            "OBJECT_RECOVERY_PROGRESS object=%s index=%u total=%u removed=%d elapsed_ms=%lld",
            ids[index],
            (unsigned)processed,
            (unsigned)count,
            removed ? 1 : 0,
            (long long)((esp_timer_get_time() - object_started_us) / 1000LL)
        );

#ifdef ESP_PLATFORM
        if (yield_between_objects && index + 1U < count) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
#else
        (void)yield_between_objects;
#endif
    }

    free(ids);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Object storage check complete processed=%u valid=%u removed=%u elapsed_ms=%lld",
            (unsigned)processed,
            (unsigned)recovered_or_valid,
            (unsigned)removed_count,
            (long long)((esp_timer_get_time() - started_us) / 1000LL)
        );
    }

    return err;
}


esp_err_t object_recovery_run(void)
{
    return object_recovery_run_cooperative(
        NULL,
        NULL,
        false
    );
}


esp_err_t object_recovery_run_active(void)
{
    char active_id[64] = {0};

    esp_err_t err = storage_manager_read_text(
        ACTIVE_OBJECT_PATH,
        active_id,
        sizeof(active_id)
    );

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No active object recovery required");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to read active object marker: %s",
            esp_err_to_name(err)
        );
        return err;
    }

    active_id[strcspn(active_id, "\r\n\t ")] = '\0';
    if (!object_format_id_is_valid(active_id)) {
        ESP_LOGW(TAG, "Active object marker is invalid; background recovery will continue");
        return ESP_OK;
    }

    const int64_t started_us = esp_timer_get_time();
    bool removed = false;
    err = object_recovery_check_guarded(active_id, &removed);

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_ACTIVE object=%s result=%d removed=%d elapsed_ms=%lld",
        active_id,
        (int)err,
        removed ? 1 : 0,
        (long long)((esp_timer_get_time() - started_us) / 1000LL)
    );

    return err;
}
