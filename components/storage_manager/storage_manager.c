#include "storage_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_manager_internal.h"


static const char *TAG =
    "STORAGE_MANAGER";


static SemaphoreHandle_t s_operation_mutex =
    NULL;


static bool s_ready =
    false;


static void storage_manager_log_boot_timing(
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


static esp_err_t storage_manager_create_mutex(void)
{
    if (s_operation_mutex != NULL) {
        return ESP_OK;
    }

    s_operation_mutex =
        xSemaphoreCreateRecursiveMutex();

    if (s_operation_mutex == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create storage operation mutex"
        );

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


esp_err_t storage_manager_init(void)
{
    esp_err_t err =
        storage_manager_create_mutex();

    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTakeRecursive(
        s_operation_mutex,
        portMAX_DELAY
    );

    if (s_ready) {
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        return ESP_OK;
    }

    struct stat mount_info;

    uint32_t stage_started_ms =
        esp_log_timestamp();

    const bool mount_available =
        stat(
            STORAGE_MANAGER_MOUNT_POINT,
            &mount_info
        ) == 0 &&
        S_ISDIR(
            mount_info.st_mode
        );

    storage_manager_log_boot_timing(
        "mount_point_stat",
        stage_started_ms
    );

    if (!mount_available) {
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        ESP_LOGE(
            TAG,
            "SD mount point is not available"
        );

        return ESP_ERR_INVALID_STATE;
    }

    stage_started_ms =
        esp_log_timestamp();

    err =
        storage_manager_ensure_directory_locked(
            STORAGE_MANAGER_ROOT_PATH
        );

    storage_manager_log_boot_timing(
        "ensure_root_directory",
        stage_started_ms
    );

    if (err == ESP_OK) {
        stage_started_ms =
            esp_log_timestamp();

        err =
            storage_manager_ensure_directory_locked(
                STORAGE_MANAGER_OBJECTS_PATH
            );

        storage_manager_log_boot_timing(
            "ensure_objects_directory",
            stage_started_ms
        );
    }

    s_ready =
        err == ESP_OK;

    xSemaphoreGiveRecursive(
        s_operation_mutex
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize storage layout"
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "Storage ready: %s",
        STORAGE_MANAGER_ROOT_PATH
    );

    return ESP_OK;
}


esp_err_t storage_manager_shutdown(void)
{
    if (s_operation_mutex == NULL) {
        return ESP_OK;
    }

    /*
     * The same recursive mutex is the high-level operation guard.
     * Shutdown therefore waits for any active storage operation.
     */
    xSemaphoreTakeRecursive(
        s_operation_mutex,
        portMAX_DELAY
    );

    s_ready =
        false;

    xSemaphoreGiveRecursive(
        s_operation_mutex
    );

    ESP_LOGI(
        TAG,
        "Storage manager stopped"
    );

    return ESP_OK;
}


bool storage_manager_is_ready(void)
{
    if (s_operation_mutex == NULL) {
        return false;
    }

    xSemaphoreTakeRecursive(
        s_operation_mutex,
        portMAX_DELAY
    );

    bool ready =
        s_ready;

    xSemaphoreGiveRecursive(
        s_operation_mutex
    );

    return ready;
}


esp_err_t storage_manager_operation_begin(void)
{
    if (s_operation_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTakeRecursive(
        s_operation_mutex,
        portMAX_DELAY
    );

    if (!s_ready) {
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


void storage_manager_operation_end(void)
{
    if (s_operation_mutex == NULL) {
        return;
    }

    xSemaphoreGiveRecursive(
        s_operation_mutex
    );
}
