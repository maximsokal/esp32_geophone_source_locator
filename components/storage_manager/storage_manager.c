#include "storage_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "spi_bus.h"
#include "storage_manager_internal.h"


#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif


static const char *TAG =
    "STORAGE_MANAGER";


/*
 * FatFs/SDSPI sometimes has to allocate a temporary 512-byte DMA-capable
 * sector buffer when its destination is not DMA-capable. Runtime TLS, Wi-Fi
 * and LVGL can otherwise fragment the internal DMA heap until that tiny
 * allocation fails with sdmmc_read_sectors: not enough mem.
 *
 * Keep one small contiguous DMA block parked while SD is idle. The outermost
 * storage operation releases it only after TFT/LVGL has been quiesced, giving
 * SDSPI a deterministic contiguous block to use. It is reacquired before the
 * display is allowed to resume.
 */
#define STORAGE_MANAGER_DMA_RESERVE_BYTES  1536U
#define STORAGE_MANAGER_DMA_REQUIRED_BYTES  512U


static SemaphoreHandle_t s_operation_mutex =
    NULL;


static bool s_ready =
    false;

static void *s_dma_reserve =
    NULL;

static unsigned s_operation_depth =
    0U;

static int64_t s_outer_operation_started_us =
    0;

static int64_t s_outer_mutex_wait_us =
    0;

static int64_t s_outer_tft_quiesce_us =
    0;


static void storage_manager_log_dma_headroom(
    const char *stage,
    bool force
)
{
    const size_t free_dma =
        heap_caps_get_free_size(MALLOC_CAP_DMA);

    const size_t largest_dma =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    if (
        !force &&
        largest_dma >= 2048U
    ) {
        return;
    }

    ESP_LOGI(
        TAG,
        "DMA_HEADROOM %s free=%u largest=%u reserve=%s depth=%u",
        stage,
        (unsigned)free_dma,
        (unsigned)largest_dma,
        s_dma_reserve != NULL ? "held" : "released",
        s_operation_depth
    );
}


static void storage_manager_dma_reserve_acquire(void)
{
    if (s_dma_reserve != NULL) {
        return;
    }

    s_dma_reserve =
        heap_caps_malloc(
            STORAGE_MANAGER_DMA_RESERVE_BYTES,
            MALLOC_CAP_DMA
        );

    if (s_dma_reserve == NULL) {
        storage_manager_log_dma_headroom(
            "reserve-allocation-failed",
            true
        );
    }
}


static void storage_manager_dma_reserve_release(void)
{
    if (s_dma_reserve == NULL) {
        return;
    }

    heap_caps_free(s_dma_reserve);
    s_dma_reserve = NULL;
}


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

    err =
        app_spi_bus_sd_operation_begin();

    if (err != ESP_OK) {
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        return err;
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
        app_spi_bus_sd_operation_end();

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

    if (s_ready) {
        storage_manager_dma_reserve_acquire();
        storage_manager_log_dma_headroom(
            "initialized",
            false
        );
    }

    app_spi_bus_sd_operation_end();

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

    s_operation_depth =
        0U;

    storage_manager_dma_reserve_release();

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

    const int64_t operation_started_us =
        esp_timer_get_time();

    xSemaphoreTakeRecursive(
        s_operation_mutex,
        portMAX_DELAY
    );

    const int64_t mutex_wait_us =
        esp_timer_get_time() - operation_started_us;
    const bool outermost = s_operation_depth == 0U;

    if (outermost) {
        s_outer_operation_started_us = operation_started_us;
        s_outer_mutex_wait_us = mutex_wait_us;
    }

    if (!s_ready) {
        s_outer_operation_started_us = 0;
        s_outer_mutex_wait_us = 0;
        s_outer_tft_quiesce_us = 0;
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (outermost) {
        const int64_t quiesce_started_us =
            esp_timer_get_time();
        err = app_spi_bus_sd_operation_begin();
        s_outer_tft_quiesce_us =
            esp_timer_get_time() - quiesce_started_us;

        if (err != ESP_OK) {
            const int64_t elapsed_us =
                esp_timer_get_time() - operation_started_us;
            const int64_t logged_mutex_wait_us =
                s_outer_mutex_wait_us;
            const int64_t logged_tft_quiesce_us =
                s_outer_tft_quiesce_us;
            const unsigned logged_depth =
                s_operation_depth;
            s_outer_operation_started_us = 0;
            xSemaphoreGiveRecursive(
                s_operation_mutex
            );

            ESP_LOGI(
                TAG,
                "STORAGE_OP_TIMING stage=begin-failed mutex_wait_us=%lld tft_quiesce_us=%lld depth=%u result=%d elapsed_us=%lld",
                (long long)logged_mutex_wait_us,
                (long long)logged_tft_quiesce_us,
                logged_depth,
                (int)err,
                (long long)elapsed_us
            );
            return err;
        }
    }

    if (outermost) {
        storage_manager_dma_reserve_release();

        const size_t largest_dma =
            heap_caps_get_largest_free_block(
                MALLOC_CAP_DMA
            );

        if (largest_dma < STORAGE_MANAGER_DMA_REQUIRED_BYTES) {
            storage_manager_log_dma_headroom(
                "sd-operation-no-sector-buffer",
                true
            );

            storage_manager_dma_reserve_acquire();
            app_spi_bus_sd_operation_end();
            const int64_t elapsed_us =
                esp_timer_get_time() - operation_started_us;
            const int64_t logged_mutex_wait_us =
                s_outer_mutex_wait_us;
            const int64_t logged_tft_quiesce_us =
                s_outer_tft_quiesce_us;
            const unsigned logged_depth =
                s_operation_depth;
            s_outer_operation_started_us = 0;
            xSemaphoreGiveRecursive(s_operation_mutex);
            ESP_LOGI(
                TAG,
                "STORAGE_OP_TIMING stage=begin-failed mutex_wait_us=%lld tft_quiesce_us=%lld depth=%u result=%d elapsed_us=%lld",
                (long long)logged_mutex_wait_us,
                (long long)logged_tft_quiesce_us,
                logged_depth,
                (int)ESP_ERR_NO_MEM,
                (long long)elapsed_us
            );
            return ESP_ERR_NO_MEM;
        }

        storage_manager_log_dma_headroom(
            "sd-operation-begin",
            false
        );
    }

    s_operation_depth++;

    if (outermost) {
        ESP_LOGI(
            TAG,
            "STORAGE_OP_TIMING stage=begin mutex_wait_us=%lld tft_quiesce_us=%lld depth=%u elapsed_us=%lld",
            (long long)s_outer_mutex_wait_us,
            (long long)s_outer_tft_quiesce_us,
            s_operation_depth,
            (long long)(esp_timer_get_time() - operation_started_us)
        );
    }

    return ESP_OK;
}


static bool storage_manager_is_web_status_cache_task(const char *name)
{
    static const char web_status_prefix[] = "web_status_cach";

    if (name == NULL) {
        return false;
    }

    return strncmp(
        name,
        web_status_prefix,
        sizeof(web_status_prefix) - 1U
    ) == 0;
}


static void storage_manager_log_owner_priority_before_release(
    int64_t elapsed_us
)
{
#if configUSE_TRACE_FACILITY == 1
    TaskStatus_t status = {0};
    vTaskGetInfo(
        NULL,
        &status,
        pdFALSE,
        eInvalid
    );

    const bool inherited =
        status.uxCurrentPriority > status.uxBasePriority;
    const bool web_status_cache =
        storage_manager_is_web_status_cache_task(status.pcTaskName);

    if (!inherited && !web_status_cache) {
        return;
    }

    ESP_LOGI(
        TAG,
        "STORAGE_PRIORITY stage=pre-release owner=%s priority_current=%u priority_base=%u inherited=%u elapsed_us=%lld",
        status.pcTaskName != NULL ? status.pcTaskName : "?",
        (unsigned)status.uxCurrentPriority,
        (unsigned)status.uxBasePriority,
        inherited ? 1U : 0U,
        (long long)elapsed_us
    );
#else
    (void)elapsed_us;
#endif
}


void storage_manager_operation_end(void)
{
    if (s_operation_mutex == NULL) {
        return;
    }

    /* An unmatched end cannot release an SPI operation it never acquired. */
    if (s_operation_depth == 0U) {
        return;
    }

    const bool outermost = s_operation_depth == 1U;
    s_operation_depth--;

    if (outermost) {
        storage_manager_dma_reserve_acquire();
        storage_manager_log_dma_headroom(
            "sd-operation-end",
            false
        );

        const int64_t resume_started_us =
            esp_timer_get_time();
        app_spi_bus_sd_operation_end();
        const int64_t resume_us =
            esp_timer_get_time() - resume_started_us;
        const int64_t elapsed_us =
            esp_timer_get_time() - s_outer_operation_started_us;
        const int64_t logged_mutex_wait_us =
            s_outer_mutex_wait_us;
        const unsigned logged_depth =
            s_operation_depth;

        storage_manager_log_owner_priority_before_release(
            elapsed_us
        );

        s_outer_operation_started_us = 0;
        xSemaphoreGiveRecursive(
            s_operation_mutex
        );

        ESP_LOGI(
            TAG,
            "STORAGE_OP_TIMING stage=end mutex_wait_us=%lld tft_resume_us=%lld depth=%u elapsed_us=%lld",
            (long long)logged_mutex_wait_us,
            (long long)resume_us,
            logged_depth,
            (long long)elapsed_us
        );
        return;
    }

    xSemaphoreGiveRecursive(
        s_operation_mutex
    );
}
