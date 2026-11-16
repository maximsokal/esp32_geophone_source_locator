#include "spi_bus.h"

#include <stdatomic.h>
#include <stddef.h>

#include "board_config.h"
#include "spi_bus_timing_log.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static const char *TAG =
    "SPI_BUS";


static SemaphoreHandle_t s_runtime_mutex =
    NULL;

static app_spi_bus_quiesce_callback_t s_display_quiesce =
    NULL;

static app_spi_bus_resume_callback_t s_display_resume =
    NULL;

static void *s_display_guard_context =
    NULL;

/* Guarded by s_runtime_mutex: touched only between a successful take and its give. */
static app_spi_bus_timing_log_t s_timing_log;
static int64_t s_operation_started_us;

/* Operations that called begin and have not yet ended or failed: running ones and those waiting for the bus. */
static atomic_uint s_sd_operations_pending;


static esp_err_t app_spi_bus_create_runtime_mutex(void)
{
    if (s_runtime_mutex != NULL) {
        return ESP_OK;
    }

    s_runtime_mutex =
        xSemaphoreCreateRecursiveMutex();

    if (s_runtime_mutex == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create SPI2 runtime arbitration mutex"
        );

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


static esp_err_t app_spi_bus_set_level_checked(
    gpio_num_t pin,
    uint32_t level,
    const char *name
)
{
    /* Keep the diagnostic label available even when log macros are compiled out. */
    (void)name;

    esp_err_t err =
        gpio_set_level(
            pin,
            level
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set %s=%u: %s",
            name,
            (unsigned)level,
            esp_err_to_name(err)
        );
    }

    return err;
}


/*
 * Put both SPI2 devices into a deterministic deselected state
 * before the bus is initialized and before the first transfer.
 *
 * AD7606 no longer shares this bus in Stage 11B. Its GPIO and
 * SPI3 lifecycle are owned by the acquisition task on Core 1.
 */
static esp_err_t app_spi_bus_prepare_device_idle_levels(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask =
            (1ULL << BOARD_TFT_CS_GPIO) |
            (1ULL << BOARD_SD_CS_GPIO),

        /*
         * INPUT_OUTPUT, not plain OUTPUT: gpio_config() disables the input
         * path for output-only pins, so gpio_get_level() on them always
         * returns 0 regardless of the driven level. The SD mount-failure
         * diagnostic reads these CS lines back, and with OUTPUT it reported
         * a permanent false "TFT_CS=0" that looked like bus contention.
         */
        .mode =
            GPIO_MODE_INPUT_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &output_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure SPI2 idle pins: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        app_spi_bus_set_level_checked(
            BOARD_TFT_CS_GPIO,
            1,
            "TFT_CS"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        app_spi_bus_set_level_checked(
            BOARD_SD_CS_GPIO,
            1,
            "SD_CS"
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Safe SPI2 boot levels: TFT_CS=1 SD_CS=1"
    );

    return ESP_OK;
}


esp_err_t app_spi_bus_init(void)
{
    /* Host log stubs may compile ESP_LOG* calls out completely. */
    (void)TAG;

    esp_err_t err =
        app_spi_bus_create_runtime_mutex();

    if (err != ESP_OK) {
        return err;
    }

    err =
        app_spi_bus_prepare_device_idle_levels();

    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num =
            BOARD_SPI_MOSI_GPIO,

        .miso_io_num =
            BOARD_SPI_MISO_GPIO,

        .sclk_io_num =
            BOARD_SPI_SCLK_GPIO,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .data4_io_num =
            -1,

        .data5_io_num =
            -1,

        .data6_io_num =
            -1,

        .data7_io_num =
            -1,

        /*
         * Buffer is large enough to transfer 40 RGB565 rows.
         */
        .max_transfer_sz =
            320 * 40 * sizeof(uint16_t),

        .flags =
            SPICOMMON_BUSFLAG_MASTER,

        .intr_flags =
            0,
    };

    ESP_LOGI(
        TAG,
        "Initializing SPI2: MOSI=%d MISO=%d SCLK=%d",
        BOARD_SPI_MOSI_GPIO,
        BOARD_SPI_MISO_GPIO,
        BOARD_SPI_SCLK_GPIO
    );

    err =
        spi_bus_initialize(
            BOARD_SPI_HOST,
            &bus_config,
            SPI_DMA_CH_AUTO
        );

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(
            TAG,
            "SPI2 bus is already initialized"
        );

        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI2 bus initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "SPI2 initialized successfully"
    );

    return ESP_OK;
}


spi_host_device_t app_spi_bus_get_host(void)
{
    return BOARD_SPI_HOST;
}


esp_err_t app_spi_bus_register_display_guard(
    app_spi_bus_quiesce_callback_t quiesce,
    app_spi_bus_resume_callback_t resume,
    void *context
)
{
    if (
        quiesce == NULL ||
        resume == NULL ||
        s_runtime_mutex == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(
        s_runtime_mutex,
        portMAX_DELAY
    );

    s_display_quiesce =
        quiesce;

    s_display_resume =
        resume;

    s_display_guard_context =
        context;

    xSemaphoreGiveRecursive(
        s_runtime_mutex
    );

    ESP_LOGI(
        TAG,
        "SPI2 runtime display/SD arbitration enabled"
    );

    return ESP_OK;
}


void app_spi_bus_unregister_display_guard(void)
{
    if (s_runtime_mutex == NULL) {
        return;
    }

    xSemaphoreTakeRecursive(
        s_runtime_mutex,
        portMAX_DELAY
    );

    s_display_quiesce =
        NULL;

    s_display_resume =
        NULL;

    s_display_guard_context =
        NULL;

    xSemaphoreGiveRecursive(
        s_runtime_mutex
    );
}


bool app_spi_bus_sd_operation_pending(void)
{
    return atomic_load_explicit(&s_sd_operations_pending, memory_order_acquire) != 0U;
}


/* An unmatched end must not wrap the counter around and leave the UI waiting for ever. */
static void app_spi_bus_sd_pending_release(void)
{
    unsigned pending = atomic_load_explicit(&s_sd_operations_pending, memory_order_relaxed);
    while (pending != 0U &&
           !atomic_compare_exchange_weak_explicit(
               &s_sd_operations_pending, &pending, pending - 1U,
               memory_order_acq_rel, memory_order_relaxed)) {
    }
}


esp_err_t app_spi_bus_sd_operation_begin(void)
{
    if (s_runtime_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Counted before the wait for the bus: an operation in the queue already makes a new UI frame a bad idea. */
    atomic_fetch_add_explicit(&s_sd_operations_pending, 1U, memory_order_acq_rel);

    const int64_t started_us =
        esp_timer_get_time();

    /* A holder that never releases SPI2 must fail this operation, not hang its task for good. */
    if (xSemaphoreTakeRecursive(
            s_runtime_mutex,
            pdMS_TO_TICKS(APP_SPI_BUS_SD_MUTEX_WAIT_MS)
        ) != pdTRUE) {
        app_spi_bus_sd_pending_release();
        ESP_LOGE(
            TAG,
            "STORAGE_SPI_TIMING stage=begin mutex_wait_us=%lld tft_quiesce_us=0 result=%d",
            (long long)(esp_timer_get_time() - started_us),
            (int)ESP_ERR_TIMEOUT
        );
        return ESP_ERR_TIMEOUT;
    }

    const int64_t mutex_wait_us =
        esp_timer_get_time() - started_us;

    if (s_display_quiesce == NULL) {
        s_operation_started_us = started_us;
        if (app_spi_bus_timing_begin(
                &s_timing_log, esp_timer_get_time(), mutex_wait_us, 0, false)) {
            ESP_LOGI(
                TAG,
                "STORAGE_SPI_TIMING stage=begin mutex_wait_us=%lld tft_quiesce_us=0 result=%d",
                (long long)mutex_wait_us,
                (int)ESP_OK
            );
        }
        (void)mutex_wait_us;
        return ESP_OK;
    }

    const int64_t quiesce_started_us =
        esp_timer_get_time();
    esp_err_t err =
        s_display_quiesce(
            s_display_guard_context
        );
    const int64_t quiesce_us =
        esp_timer_get_time() - quiesce_started_us;

    s_operation_started_us = started_us;
    const bool log_begin = app_spi_bus_timing_begin(
        &s_timing_log, esp_timer_get_time(), mutex_wait_us, quiesce_us, err != ESP_OK);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to quiesce TFT before SD access: %s",
            esp_err_to_name(err)
        );

        xSemaphoreGiveRecursive(
            s_runtime_mutex
        );
        app_spi_bus_sd_pending_release();
    }

    if (log_begin) {
        ESP_LOGI(
            TAG,
            "STORAGE_SPI_TIMING stage=begin mutex_wait_us=%lld tft_quiesce_us=%lld result=%d",
            (long long)mutex_wait_us,
            (long long)quiesce_us,
            (int)err
        );
    }
    (void)mutex_wait_us;
    (void)quiesce_us;

    return err;
}


void app_spi_bus_sd_operation_end(void)
{
    if (s_runtime_mutex == NULL) {
        return;
    }

    const int64_t started_us =
        esp_timer_get_time();
    if (s_display_resume != NULL) {
        s_display_resume(
            s_display_guard_context
        );
    }
    const int64_t finished_us =
        esp_timer_get_time();
    const int64_t resume_us =
        finished_us - started_us;
    /* elapsed_us keeps its meaning (the end stage); the summary reports the whole operation. */
    const int64_t operation_us =
        finished_us - s_operation_started_us;
    bool summary_due = false;
    app_spi_bus_timing_summary_t summary = {0};
    const bool log_end = app_spi_bus_timing_end(
        &s_timing_log, finished_us, resume_us, operation_us, &summary_due, &summary);

    xSemaphoreGiveRecursive(
        s_runtime_mutex
    );
    app_spi_bus_sd_pending_release();

    if (log_end) {
        ESP_LOGI(
            TAG,
            "STORAGE_SPI_TIMING stage=end tft_resume_us=%lld elapsed_us=%lld",
            (long long)resume_us,
            (long long)(esp_timer_get_time() - started_us)
        );
    }
    if (summary_due) {
        ESP_LOGI(
            TAG,
            "STORAGE_SPI_SUMMARY ops=%u failed=%u window_ms=%lld max_mutex_wait_us=%lld max_tft_quiesce_us=%lld max_tft_resume_us=%lld max_operation_us=%lld",
            (unsigned)summary.operations,
            (unsigned)summary.failures,
            (long long)(summary.window_us / 1000LL),
            (long long)summary.max_mutex_wait_us,
            (long long)summary.max_quiesce_us,
            (long long)summary.max_resume_us,
            (long long)summary.max_elapsed_us
        );
    }
    (void)resume_us;
}
