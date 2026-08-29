#include "geophone_adc_test.h"

#include "sdkconfig.h"

#include "geophone_adc_test_internal.h"

#include "adc_service.h"
#include "app_task_config.h"
#include "board_config.h"
#include "seismic_buffer.h"

#include "esp_log.h"


static const char *TAG =
    "GEOPHONE_ADC";


TaskHandle_t g_geophone_adc_task =
    NULL;

esp_timer_handle_t g_geophone_adc_timer =
    NULL;

adc_channel_t g_geophone_adc_channel;

adc_cali_handle_t g_geophone_adc_cali =
    NULL;

volatile bool g_geophone_adc_running =
    false;


esp_err_t geophone_adc_test_start(void)
{
    if (g_geophone_adc_task != NULL) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(
        adc_service_init()
    );

    ESP_ERROR_CHECK(
        geophone_adc_prepare_hardware()
    );

    ESP_ERROR_CHECK(
        seismic_buffer_configure(
            SEISMIC_SOURCE_ESP_ADC_BENCH,
            CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ
        )
    );

    g_geophone_adc_running =
        true;

    BaseType_t result =
        xTaskCreatePinnedToCore(
            geophone_adc_worker,
            "geophone_adc",
            APP_TASK_STACK_GEOPHONE_ADC,
            NULL,
            APP_TASK_PRIORITY_GEOPHONE_ADC,
            &g_geophone_adc_task,
            APP_TASK_CORE_REALTIME
        );

    if (result != pdPASS) {
        g_geophone_adc_running =
            false;

        geophone_adc_release_hardware();

        return ESP_ERR_NO_MEM;
    }

    const uint64_t period_us =
        1000000ULL /
        CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ;

    esp_err_t err =
        esp_timer_start_periodic(
            g_geophone_adc_timer,
            period_us
        );

    if (err != ESP_OK) {
        geophone_adc_test_stop();

        return err;
    }

    ESP_LOGW(
        TAG,
        "ESP ADC bench enabled: GPIO%d at %d Hz core=%d",
        BOARD_GEOPHONE_BENCH_GPIO,
        CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ,
        APP_TASK_CORE_REALTIME
    );

    return ESP_OK;
}


void geophone_adc_test_stop(void)
{
    if (g_geophone_adc_task == NULL) {
        return;
    }

    if (g_geophone_adc_timer != NULL) {
        (void)esp_timer_stop(
            g_geophone_adc_timer
        );
    }

    g_geophone_adc_running =
        false;

    xTaskNotifyGive(
        g_geophone_adc_task
    );

    while (g_geophone_adc_task != NULL) {
        vTaskDelay(
            pdMS_TO_TICKS(10)
        );
    }

    geophone_adc_release_hardware();

    ESP_LOGI(
        TAG,
        "ESP ADC bench stopped"
    );
}
