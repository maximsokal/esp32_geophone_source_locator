#include "geophone_adc_test_internal.h"

#include <stddef.h>
#include <stdint.h>

#include "adc_service.h"
#include "seismic_buffer.h"

#include "esp_log.h"
#include "esp_timer.h"


static const char *TAG =
    "GEOPHONE_ADC";


#define GEOPHONE_BASELINE_SAMPLES    256


void geophone_adc_timer_callback(
    void *argument
)
{
    (void)argument;

    if (
        g_geophone_adc_running &&
        g_geophone_adc_task != NULL
    ) {
        xTaskNotifyGive(
            g_geophone_adc_task
        );
    }
}


void geophone_adc_worker(
    void *argument
)
{
    (void)argument;

    int64_t baseline_sum =
        0;

    size_t baseline_count =
        0;

    float baseline_raw =
        0.0f;

    float uv_per_count =
        0.0f;

    while (g_geophone_adc_running) {
        (void)ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        if (!g_geophone_adc_running) {
            break;
        }

        int raw =
            0;

        esp_err_t err =
            adc_service_read_raw(
                g_geophone_adc_channel,
                &raw
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "ADC read failed: %s",
                esp_err_to_name(err)
            );

            continue;
        }

        if (
            baseline_count <
            GEOPHONE_BASELINE_SAMPLES
        ) {
            baseline_sum +=
                raw;

            baseline_count++;

            if (
                baseline_count ==
                GEOPHONE_BASELINE_SAMPLES
            ) {
                baseline_raw =
                    (float)baseline_sum /
                    GEOPHONE_BASELINE_SAMPLES;

                err =
                    geophone_adc_calibrate_scale(
                        baseline_raw,
                        &uv_per_count
                    );

                if (err != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "ADC scale calibration failed: %s",
                        esp_err_to_name(err)
                    );

                    break;
                }

                ESP_LOGI(
                    TAG,
                    "Baseline raw=%.1f, scale=%.1f uV/count",
                    baseline_raw,
                    uv_per_count
                );
            }

            continue;
        }

        const float signal_uv =
            (
                (float)raw -
                baseline_raw
            ) *
            uv_per_count;

        const float channels[
            SEISMIC_CHANNEL_COUNT
        ] = {
            signal_uv,
            0.0f,
            0.0f,
            0.0f,
        };

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
    }

    g_geophone_adc_running =
        false;

    g_geophone_adc_task =
        NULL;

    vTaskDelete(
        NULL
    );
}
