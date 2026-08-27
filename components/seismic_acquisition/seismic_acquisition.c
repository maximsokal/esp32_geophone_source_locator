#include "seismic_acquisition.h"

#include "sdkconfig.h"

#include "geophone_adc_test.h"
#include "seismic_test_generator.h"

#include "esp_log.h"


static const char *TAG =
    "SEISMIC_ACQ";


esp_err_t seismic_acquisition_start(void)
{
#if CONFIG_SEISMIC_SOURCE_TEST
    ESP_LOGI(
        TAG,
        "Source: TEST"
    );

    return
        seismic_test_generator_start();

#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    ESP_LOGI(
        TAG,
        "Source: ESP_ADC_BENCH"
    );

    return
        geophone_adc_test_start();

#else
#error "No seismic acquisition source selected"
#endif
}


void seismic_acquisition_stop(void)
{
#if CONFIG_SEISMIC_SOURCE_TEST
    seismic_test_generator_stop();

#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    geophone_adc_test_stop();
#endif
}


const char *seismic_acquisition_source_name(void)
{
#if CONFIG_SEISMIC_SOURCE_TEST
    return "TEST";

#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    return "ESP_ADC_BENCH";

#else
    return "NONE";
#endif
}
