#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "object_store_internal.h"

#include <stdio.h>

#include "storage_manager.h"


static SemaphoreHandle_t s_mutex =
    NULL;


static bool s_initialized =
    false;


static char s_active_object_id[
    OBJECT_STORE_ID_SIZE
] = "";


bool object_store_internal_is_initialized(void)
{
    return
        s_initialized;
}


void object_store_internal_set_initialized(
    bool initialized
)
{
    s_initialized =
        initialized;
}


void *object_store_internal_mutex_handle(void)
{
    return
        (void *)s_mutex;
}


void object_store_internal_set_mutex_handle(
    void *mutex
)
{
    s_mutex =
        (SemaphoreHandle_t)mutex;
}


esp_err_t object_store_internal_begin_state_operation(void)
{
    if (
        !s_initialized ||
        s_mutex == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    return ESP_OK;
}


void object_store_internal_end_state_operation(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(
            s_mutex
        );
    }
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
    object_store_internal_end_state_operation();
    storage_manager_operation_end();
}


const char *object_store_internal_active_id(void)
{
    return
        s_active_object_id;
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
