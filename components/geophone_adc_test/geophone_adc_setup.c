#include "geophone_adc_test_internal.h"

#include "sdkconfig.h"

#include "adc_service.h"
#include "board_config.h"

#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"


static const char *TAG =
    "GEOPHONE_ADC";


esp_err_t geophone_adc_prepare_hardware(void)
{
    esp_err_t err =
        adc_service_configure_gpio(
            BOARD_GEOPHONE_BENCH_GPIO,
            ADC_ATTEN_DB_12,
            ADC_BITWIDTH_DEFAULT,
            &g_geophone_adc_channel
        );

    if (err != ESP_OK) {
        return err;
    }

    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = adc_service_unit(),
        .chan = g_geophone_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    err =
        adc_cali_create_scheme_curve_fitting(
            &cali,
            &g_geophone_adc_cali
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ADC curve calibration unavailable: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    const esp_timer_create_args_t timer = {
        .callback = geophone_adc_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "geo_adc_tick",
    };

    err =
        esp_timer_create(
            &timer,
            &g_geophone_adc_timer
        );

    if (err != ESP_OK) {
        adc_cali_delete_scheme_curve_fitting(
            g_geophone_adc_cali
        );

        g_geophone_adc_cali =
            NULL;
    }

    return err;
}


void geophone_adc_release_hardware(void)
{
    if (g_geophone_adc_timer != NULL) {
        esp_timer_delete(
            g_geophone_adc_timer
        );

        g_geophone_adc_timer =
            NULL;
    }

    if (g_geophone_adc_cali != NULL) {
        adc_cali_delete_scheme_curve_fitting(
            g_geophone_adc_cali
        );

        g_geophone_adc_cali =
            NULL;
    }
}
