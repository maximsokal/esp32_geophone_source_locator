#include "adc_service.h"

#include "adc_service_internal.h"

#include "esp_log.h"


static const char *TAG =
    "ADC_SERVICE";


adc_oneshot_unit_handle_t g_adc_service_handle =
    NULL;

SemaphoreHandle_t g_adc_service_mutex =
    NULL;

bool g_adc_service_initialized =
    false;


esp_err_t adc_service_init(void)
{
    if (g_adc_service_initialized) {
        return ESP_OK;
    }

    g_adc_service_mutex =
        xSemaphoreCreateMutex();

    if (g_adc_service_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const adc_oneshot_unit_init_cfg_t config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };

    esp_err_t err =
        adc_oneshot_new_unit(
            &config,
            &g_adc_service_handle
        );

    if (err != ESP_OK) {
        vSemaphoreDelete(
            g_adc_service_mutex
        );

        g_adc_service_mutex =
            NULL;

        return err;
    }

    g_adc_service_initialized =
        true;

    ESP_LOGI(
        TAG,
        "ADC1 shared service initialized"
    );

    return ESP_OK;
}


adc_oneshot_unit_handle_t *adc_service_handle(void)
{
    return &g_adc_service_handle;
}


adc_unit_t adc_service_unit(void)
{
    return ADC_UNIT_1;
}
