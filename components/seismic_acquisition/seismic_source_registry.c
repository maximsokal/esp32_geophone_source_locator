#include "seismic_source_driver.h"

#include "sdkconfig.h"


static const seismic_source_driver_t *const s_registry[] = {
    &g_seismic_source_test,
    &g_seismic_source_esp_adc,
    &g_seismic_source_ad7606,
};


const seismic_source_driver_t *seismic_source_registry_selected(void)
{
#if CONFIG_SEISMIC_SOURCE_TEST
    return s_registry[0];
#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
    return s_registry[1];
#elif CONFIG_SEISMIC_SOURCE_AD7606
    return s_registry[2];
#else
#error "No seismic acquisition source selected"
#endif
}
