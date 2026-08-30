#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "seismic_acquisition.h"


typedef struct
{
    const char *name;
    const char *transport_name;

    uint32_t sample_rate_hz;
    uint8_t channel_count;
    int core_id;

    esp_err_t (*start)(void);
    void (*stop)(void);
    esp_err_t (*populate_snapshot)(
        seismic_acquisition_snapshot_t *snapshot
    );

} seismic_source_driver_t;


#if CONFIG_SEISMIC_SOURCE_TEST
extern const seismic_source_driver_t
    g_seismic_source_test;
#elif CONFIG_SEISMIC_SOURCE_ESP_ADC_BENCH
extern const seismic_source_driver_t
    g_seismic_source_esp_adc;
#elif CONFIG_SEISMIC_SOURCE_AD7606
extern const seismic_source_driver_t
    g_seismic_source_ad7606;
#else
#error "No seismic acquisition source selected"
#endif


const seismic_source_driver_t *seismic_source_registry_selected(void);

esp_err_t seismic_source_snapshot_from_buffer(
    seismic_acquisition_snapshot_t *snapshot
);
