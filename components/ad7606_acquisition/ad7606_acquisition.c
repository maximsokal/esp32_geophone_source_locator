#include "ad7606_acquisition.h"

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "ad7606_driver.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "AD7606_ACQ";


static TaskHandle_t s_task =
    NULL;

static esp_timer_handle_t s_timer =
    NULL;

static volatile bool s_running =
    false;


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


static void ad7606_worker(
    void *argument
)
{
    (void)argument;

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

        ad7606_sample_t sample;

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
                    "Read failed: %s (check BUSY/CONVST/SPI wiring)",
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

        err =
            seismic_buffer_push(
                esp_timer_get_time(),
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

    s_running =
        false;

    s_task =
        NULL;

    vTaskDelete(
        NULL
    );
}


esp_err_t ad7606_acquisition_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

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
        ad7606_driver_deinit();

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
        ad7606_driver_deinit();

        return err;
    }

    s_running =
        true;

    BaseType_t result =
        xTaskCreate(
            ad7606_worker,
            "ad7606_acq",
            4096,
            NULL,
            7,
            &s_task
        );

    if (result != pdPASS) {
        s_running =
            false;

        esp_timer_delete(
            s_timer
        );

        s_timer =
            NULL;

        ad7606_driver_deinit();

        return ESP_ERR_NO_MEM;
    }

    const uint64_t period_us =
        1000000ULL /
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ;

    err =
        esp_timer_start_periodic(
            s_timer,
            period_us
        );

    if (err != ESP_OK) {
        ad7606_acquisition_stop();

        return err;
    }

    ESP_LOGI(
        TAG,
        "Started: %d Hz, 8 channels, +/-5 V expected",
        CONFIG_SEISMIC_AD7606_SAMPLE_RATE_HZ
    );

    return ESP_OK;
}


void ad7606_acquisition_stop(void)
{
    if (
        s_task == NULL &&
        s_timer == NULL
    ) {
        ad7606_driver_deinit();

        return;
    }

    if (s_timer != NULL) {
        (void)esp_timer_stop(
            s_timer
        );
    }

    s_running =
        false;

    if (s_task != NULL) {
        xTaskNotifyGive(
            s_task
        );

        while (s_task != NULL) {
            vTaskDelay(
                pdMS_TO_TICKS(10)
            );
        }
    }

    if (s_timer != NULL) {
        (void)esp_timer_delete(
            s_timer
        );

        s_timer =
            NULL;
    }

    ad7606_driver_deinit();

    ESP_LOGI(
        TAG,
        "Stopped"
    );
}