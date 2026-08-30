#include "seismic_source_driver.h"


const seismic_source_driver_t *seismic_source_registry_selected(void)
{
#if CONFIG_SEISMIC_SOURCE_TEST
    return &g_seismic_source_test;
#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    return &g_seismic_source_esp_adc;
#elif CONFIG_SEISMIC_SOURCE_AD7606
    return &g_seismic_source_ad7606;
#else
#error "No seismic acquisition source selected"
#endif
}
