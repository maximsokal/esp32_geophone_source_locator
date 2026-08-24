#include "object_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "object_store_internal.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG =
    "OBJECT_STORE";


static SemaphoreHandle_t s_mutex =
    NULL;


static bool s_initialized =
    false;


static char s_active_object_id[
    OBJECT_STORE_ID_SIZE
] = "";


static void object_store_fill_summary(
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


    s_active_object_id[0] =
        '\0';


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
            snprintf(
                s_active_object_id,
                sizeof(s_active_object_id),
                "%s",
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
        err == ESP_ERR_NOT_FOUND
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


    s_initialized =
        true;


    return ESP_OK;
}


esp_err_t object_store_create(
    object_store_summary_t *created_object
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    char object_id[
        OBJECT_STORE_ID_SIZE
    ];


    esp_err_t err =
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

        snprintf(
            s_active_object_id,
            sizeof(s_active_object_id),
            "%s",
            object_id
        );


        if (created_object != NULL) {

            object_store_fill_summary(
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


    xSemaphoreGive(
        s_mutex
    );


    return err;
}


esp_err_t object_store_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    if (
        objects == NULL ||
        object_count == NULL ||
        max_objects == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    esp_err_t err =
        object_store_fs_list(
            objects,
            max_objects,
            object_count,
            s_active_object_id
        );


    xSemaphoreGive(
        s_mutex
    );


    return err;
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


    object_store_fill_summary(
        object,
        s_active_object_id,
        true
    );


    xSemaphoreGive(
        s_mutex
    );


    return ESP_OK;
}


esp_err_t object_store_set_active(
    const char *object_id
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    if (
        !object_store_fs_id_is_valid(
            object_id
        )
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    if (
        !object_store_fs_object_exists(
            object_id
        )
    ) {
        xSemaphoreGive(
            s_mutex
        );

        return ESP_ERR_NOT_FOUND;
    }


    esp_err_t err =
        object_store_fs_save_active(
            object_id
        );


    if (err == ESP_OK) {

        snprintf(
            s_active_object_id,
            sizeof(s_active_object_id),
            "%s",
            object_id
        );


        ESP_LOGI(
            TAG,
            "Active object: %s",
            object_id
        );
    }


    xSemaphoreGive(
        s_mutex
    );


    return err;
}