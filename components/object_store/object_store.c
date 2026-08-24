#include "object_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "object_store_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_manager.h"


static const char *TAG =
    "OBJECT_STORE";


static SemaphoreHandle_t s_mutex =
    NULL;


static bool s_initialized =
    false;


static char s_active_object_id[
    OBJECT_STORE_ID_SIZE
] = "";


void object_store_internal_fill_summary(
    object_store_summary_t *object,
    const char *object_id,
    bool active
)
{
    snprintf(
        object->id,
        sizeof(object->id),
        "%s",
        object_id
    );

    snprintf(
        object->name,
        sizeof(object->name),
        "%s",
        object_id
    );

    object->active =
        active;
}


esp_err_t object_store_internal_begin_file_operation(void)
{
    if (
        !s_initialized ||
        s_mutex == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        storage_manager_operation_begin();

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    return ESP_OK;
}


void object_store_internal_end_file_operation(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(
            s_mutex
        );
    }

    storage_manager_operation_end();
}


const char *object_store_internal_active_id(void)
{
    return s_active_object_id;
}


void object_store_internal_set_active_id(
    const char *object_id
)
{
    snprintf(
        s_active_object_id,
        sizeof(s_active_object_id),
        "%s",
        object_id
    );
}


void object_store_internal_clear_active_id(void)
{
    s_active_object_id[0] =
        '\0';
}


esp_err_t object_store_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex =
        xSemaphoreCreateMutex();

    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t guard_err =
        storage_manager_operation_begin();

    if (guard_err != ESP_OK) {
        vSemaphoreDelete(
            s_mutex
        );

        s_mutex =
            NULL;

        return guard_err;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    object_store_internal_clear_active_id();

    char active_id[
        OBJECT_STORE_ID_SIZE
    ];

    esp_err_t err =
        object_store_fs_load_active(
            active_id,
            sizeof(active_id)
        );

    if (err == ESP_OK) {
        if (
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
                s_active_object_id
            );
        }
        else {
            ESP_LOGW(
                TAG,
                "Active object does not exist: %s",
                active_id
            );
        }
    }
    else if (
        err ==
        ESP_ERR_NOT_FOUND
    ) {
        ESP_LOGI(
            TAG,
            "No active object"
        );
    }
    else {
        ESP_LOGW(
            TAG,
            "Active object marker is invalid"
        );
    }

    xSemaphoreGive(
        s_mutex
    );

    storage_manager_operation_end();

    s_initialized =
        true;

    return ESP_OK;
}


esp_err_t object_store_get_active(
    object_store_summary_t *object
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (object == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    if (
        s_active_object_id[0] ==
        '\0'
    ) {
        xSemaphoreGive(
            s_mutex
        );

        return ESP_ERR_NOT_FOUND;
    }

    object_store_internal_fill_summary(
        object,
        s_active_object_id,
        true
    );

    xSemaphoreGive(
        s_mutex
    );

    return ESP_OK;
}


esp_err_t object_store_reset(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    object_store_internal_clear_active_id();

    xSemaphoreGive(
        s_mutex
    );

    ESP_LOGI(
        TAG,
        "Runtime object state reset"
    );

    return ESP_OK;
}
