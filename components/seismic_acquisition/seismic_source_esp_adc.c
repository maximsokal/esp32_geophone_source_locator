#include "seismic_source_driver.h"

#include "sdkconfig.h"

#include "app_task_config.h"
#include "geophone_adc_test.h"
#include "seismic_buffer.h"


const seismic_source_driver_t g_seismic_source_esp_adc = {
    .name = "ESP_ADC_BENCH",
    .transport_name = "ADC1",
    .sample_rate_hz = CONFIG_SEISMIC_ADC_BENCH_SAMPLE_RATE_HZ,
    .channel_count = SEISMIC_CHANNEL_COUNT,
    .core_id = APP_TASK_CORE_REALTIME,
    .start = geophone_adc_test_start,
    .stop = geophone_adc_test_stop,
    .populate_snapshot = seismic_source_snapshot_from_buffer,
};
