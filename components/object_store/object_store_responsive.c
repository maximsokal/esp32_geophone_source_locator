#include "object_store.h"

#include <stdlib.h>

#include "object_store_internal.h"

#include "esp_log.h"
#include "esp_timer.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


static const char *TAG = "OBJECT_STORE_RESPONSIVE";


esp_err_t object_store_list_profiles_responsive(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    if (
        objects == NULL ||
        object_count == NULL ||
        default_cable_lengths_m == NULL ||
        max_objects == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0U;
    const size_t bounded =
        max_objects < OBJECT_STORE_MAX_OBJECTS
            ? max_objects
            : OBJECT_STORE_MAX_OBJECTS;

    object_store_summary_t *summaries = calloc(
        bounded,
        sizeof(*summaries)
    );
    if (summaries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t started_us = esp_timer_get_time();
    size_t census_count = 0U;

    /*
     * Take only the cheap directory census while storage/TFT arbitration is
     * held. Metadata reads are deliberately outside this operation and each
     * object gets its own bounded object_store_get_profile() storage window.
     */
    esp_err_t err = object_store_internal_begin_file_operation();
    if (err == ESP_OK) {
        err = object_store_fs_list(
            summaries,
            bounded,
            &census_count,
            object_store_internal_active_id()
        );
        object_store_internal_end_file_operation();
    }

    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    err = object_store_get_cable_defaults(
        default_cable_lengths_m
    );
    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    size_t published = 0U;
    size_t omitted = 0U;
    for (size_t i = 0U; i < census_count; ++i) {
        object_store_profile_t profile;
        err = object_store_get_profile(
            summaries[i].id,
            &profile
        );

        /* Deletion can race the census, and background recovery may still be
         * processing a parseable-but-semantic-invalid object. Both are local
         * archive entries to omit, not reasons to fail the whole snapshot. */
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE) {
            ++omitted;
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            break;
        }

        objects[published++] = profile;

#ifdef ESP_PLATFORM
        /* Explicitly let LVGL/input/network service work run between FATFS
         * metadata windows. The task itself is low priority, but FATFS/newlib
         * mutexes can temporarily raise its current priority via inheritance. */
        if (i + 1U < census_count) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
#endif
    }

    if (err == ESP_OK) {
        *object_count = published;
        ESP_LOGI(
            TAG,
            "PROFILE_SCAN mode=cooperative census=%u published=%u omitted=%u elapsed_ms=%lld",
            (unsigned)census_count,
            (unsigned)published,
            (unsigned)omitted,
            (long long)((esp_timer_get_time() - started_us) / 1000LL)
        );
    }
    else {
        *object_count = 0U;
        ESP_LOGW(
            TAG,
            "PROFILE_SCAN mode=cooperative failed_at=%u err=%s elapsed_ms=%lld",
            (unsigned)published,
            esp_err_to_name(err),
            (long long)((esp_timer_get_time() - started_us) / 1000LL)
        );
    }

    free(summaries);
    return err;
}
