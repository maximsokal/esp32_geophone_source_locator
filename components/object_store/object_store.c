#include "object_store.h"

#include "object_store_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_manager.h"


static const char *TAG =
    "OBJECT_STORE";


esp_err_t object_store_init(void)
{
    if (object_store_internal_is_initialized()) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    object_store_internal_set_mutex_handle((void *)mutex);

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        vSemaphoreDelete(mutex);
        object_store_internal_set_mutex_handle(NULL);
        return err;
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    object_store_profile_cache_reset();
    object_store_internal_clear_active_id();
    object_store_internal_restore_active();
    xSemaphoreGive(mutex);

    storage_manager_operation_end();
    object_store_internal_set_initialized(true);
    return ESP_OK;
}


esp_err_t object_store_get_active(object_store_summary_t *object)
{
    if (!object_store_internal_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t mutex =
        (SemaphoreHandle_t)object_store_internal_mutex_handle();
    xSemaphoreTake(mutex, portMAX_DELAY);

    const char *active_id = object_store_internal_active_id();
    if (active_id[0] == '\0') {
        xSemaphoreGive(mutex);
        return ESP_ERR_NOT_FOUND;
    }

    object_store_internal_fill_summary(object, active_id, true);
    xSemaphoreGive(mutex);
    return ESP_OK;
}


esp_err_t object_store_invalidate_profile_cache(void)
{
    if (!object_store_internal_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t mutex =
        (SemaphoreHandle_t)object_store_internal_mutex_handle();
    xSemaphoreTake(mutex, portMAX_DELAY);

    /* RAM-only. The settings cache is cheap to refill and resetting it with
     * the profile index avoids exposing a second cache-reset primitive while
     * leaving persisted/runtime ACTIVE completely untouched. */
    object_store_profile_cache_reset();

    xSemaphoreGive(mutex);

    ESP_LOGI(TAG, "Object profile cache invalidated after recovery");
    return ESP_OK;
}


esp_err_t object_store_reset(void)
{
    if (!object_store_internal_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t mutex =
        (SemaphoreHandle_t)object_store_internal_mutex_handle();
    xSemaphoreTake(mutex, portMAX_DELAY);

    object_store_internal_clear_active_id();
    object_store_profile_cache_note_active_locked("");

    xSemaphoreGive(mutex);

    ESP_LOGI(TAG, "Runtime object state reset");
    return ESP_OK;
}
