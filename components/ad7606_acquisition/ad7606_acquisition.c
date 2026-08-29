#include "ad7606_acquisition.h"

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "ad7606_driver.h"
#include "app_task_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


static const char *TAG =
    "AD7606_ACQ";


static TaskHandle_t s_task =
    NULL;

static esp_timer_handle_t s_timer =
    NULL;

static volatile bool s_running =
    false;


typedef struct
{
    SemaphoreHandle_t ready;
    esp_err_t result;

} ad7606_start_context_t;


static void ad7606_timer_callback(
    void *argument
)
{
    (void)argument;

    if (
        s_running &&
        s_task != NULL
    ) {
        xTaskNotifyGive(
            s_task
        );
    }
}


static void ad7606_log_sample(
    const ad7606_sample_t *sample
)
{
    ESP_LOGI(
        TAG,
        "raw=[%d,%d,%d,%d,%d,%d,%d,%d]",
        (int)sample->raw[0],
        (int)sample->raw[1],
        (int)sample->raw[2],
        (int)sample->raw[3],
        (int)sample->raw[4],
        (int)sample->raw[5],
        (int)sample->raw[6],
        (int)sample->raw[7]
    );

    ESP_LOGI(
        TAG,
        "mV=[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]",
        ad7606_raw_to_uv(
            sample->raw[0]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[1]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[2]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[3]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[4]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[5]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[6]
        ) / 1000.0f,
        ad7606_raw_to_uv(
            sample->raw[7]
        ) / 1000.0f
    );
}


static void ad7606_worker_cleanup(void)
{
    if (s_timer != NULL) {
        (void)esp_timer_stop(
            s_timer
        );

        (void)esp_timer_delete(
            s_timer
        );

        s_timer =
            NULL;
    }

    /*
     * The driver owns SPI3, so deinit must remain on this same
     * Core 1 worker that initialized the bus.
     */
    ad7606_driver_deinit();

    (void)seismic_buffer_configure(
        SEISMIC_SOURCE_NONE,
        0
    );

    s_running =
        false;
}


static esp_err_t ad7606_worker_initialize(void)
{
    /*
     * This function executes inside the Core 1 acquisition task.
     * Consequently spi_bus_initialize(SPI3_HOST) also runs on
     * Core 1 and the SPI3 ISR affinity stays with the real-time
     * acquisition side of the application.
     */
    esp_err_t err =
        ad7606_driver_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        seismic_buffer_configure(
            SEISMIC_SOURCE_AD7606,
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
        );

    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t timer_config = {
        .callback =
            ad7606_timer_callback,

        .arg =
            NULL,

        .dispatch_method =
            ESP_TIMER_TASK,

        .name =
            "ad7606_tick",
    };

    err =
        esp_timer_create(
            &timer_config,
            &s_timer
        );

    if (err != ESP_OK) {
        return err;
    }

    const uint64_t period_us =
        1000000ULL /
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;

    s_running =
        true;

    err =
        esp_timer_start_periodic(
            s_timer,
            period_us
        );

    if (err != ESP_OK) {
        s_running =
            false;

        return err;
    }

    ESP_LOGI(
        TAG,
        "Started: %d Hz, 8 channels, SPI3 dedicated, core=%d",
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ,
        APP_TASK_CORE_REALTIME
    );

    return ESP_OK;
}


static void ad7606_worker(
    void *argument
)
{
    ad7606_start_context_t *start_context =
        (ad7606_start_context_t *)argument;

    s_task =
        xTaskGetCurrentTaskHandle();

    esp_err_t startup_err =
        ad7606_worker_initialize();

    if (startup_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Core 1 acquisition initialization failed: %s",
            esp_err_to_name(startup_err)
        );

        ad7606_worker_cleanup();

        s_task =
            NULL;

        start_context->result =
            startup_err;

        xSemaphoreGive(
            start_context->ready
        );

        vTaskDelete(
            NULL
        );

        return;
    }

    start_context->result =
        ESP_OK;

    xSemaphoreGive(
        start_context->ready
    );

    /*
     * The start context belongs to the caller's stack and must
     * not be touched after the ready semaphore is released.
     */
    start_context =
        NULL;

    uint32_t successful_samples =
        0;

    uint32_t consecutive_failures =
        0;

    while (s_running) {
        (void)ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        if (!s_running) {
            break;
        }

        ad7606_sample_t sample = {0};

        esp_err_t err =
            ad7606_driver_read(
                &sample
            );

        if (err != ESP_OK) {
            consecutive_failures++;

            if (
                consecutive_failures == 1 ||
                (
                    consecutive_failures %
                    CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
                ) == 0
            ) {
                ESP_LOGW(
                    TAG,
                    "Read failed: %s (check BUSY/CONVST/SPI3 wiring)",
                    esp_err_to_name(err)
                );
            }

            continue;
        }

        consecutive_failures =
            0;

        float channels[
            SEISMIC_CHANNEL_COUNT
        ];

        for (
            size_t channel = 0;
            channel < SEISMIC_CHANNEL_COUNT;
            ++channel
        ) {
            channels[channel] =
                ad7606_raw_to_uv(
                    sample.raw[channel]
                );
        }

        /*
         * timestamp_us now belongs to the actual simultaneous
         * CONVST event, not the later end of the SPI read.
         */
        err =
            seismic_buffer_push(
                sample.timestamp_us,
                channels
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Seismic buffer push failed: %s",
                esp_err_to_name(err)
            );
        }

        successful_samples++;

        if (
            successful_samples %
            CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ ==
            0
        ) {
            ad7606_log_sample(
                &sample
            );
        }
    }

    ad7606_worker_cleanup();

    s_task =
        NULL;

    ESP_LOGI(
        TAG,
        "Stopped"
    );

    vTaskDelete(
        NULL
    );
}


esp_err_t ad7606_acquisition_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    ad7606_start_context_t start_context = {
        .ready =
            xSemaphoreCreateBinary(),

        .result =
            ESP_ERR_INVALID_STATE,
    };

    if (start_context.ready == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result =
        xTaskCreatePinnedToCore(
            ad7606_worker,
            "ad7606_acq",
            APP_TASK_STACK_AD7606,
            &start_context,
            APP_TASK_PRIORITY_AD7606,
            &s_task,
            APP_TASK_CORE_REALTIME
        );

    if (task_result != pdPASS) {
        vSemaphoreDelete(
            start_context.ready
        );

        s_task =
            NULL;

        return ESP_ERR_NO_MEM;
    }

    /*
     * Keep the public start() contract synchronous: startup does
     * not report success until Core 1 has initialized SPI3,
     * self-tested the ADC and started the sampling timer.
     */
    (void)xSemaphoreTake(
        start_context.ready,
        portMAX_DELAY
    );

    const esp_err_t result =
        start_context.result;

    vSemaphoreDelete(
        start_context.ready
    );

    return result;
}


void ad7606_acquisition_stop(void)
{
    TaskHandle_t task =
        s_task;

    if (task == NULL) {
        return;
    }

    s_running =
        false;

    xTaskNotifyGive(
        task
    );

    /*
     * Worker cleanup includes timer deletion, AD7606 deinit and
     * SPI3 bus release on Core 1.
     */
    while (s_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }
}
