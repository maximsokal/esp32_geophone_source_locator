#include "storage_bulk_admission.h"
#include "storage_manager_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static bool s_protected_active;


esp_err_t storage_bulk_admission_initialize(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}


esp_err_t storage_bulk_admission_enter_protected(void)
{
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    s_protected_active = true;
    (void)xSemaphoreGive(s_mutex);
    return ESP_OK;
}


void storage_bulk_admission_exit_protected(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_protected_active = false;
    (void)xSemaphoreGive(s_mutex);
}


bool storage_bulk_admission_is_protected(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    const bool is_protected = s_protected_active;
    (void)xSemaphoreGive(s_mutex);
    return is_protected;
}


esp_err_t storage_bulk_admission_run_chunk(
    esp_err_t (*fn)(void *),
    void *ctx
)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_protected_active) {
        err = fn(ctx);
    }

    (void)xSemaphoreGive(s_mutex);
    return err;
}
