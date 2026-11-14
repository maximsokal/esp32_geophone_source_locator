#include "seismic_source_driver.h"

#include "app_task_config.h"
#include "seismic_buffer.h"
#include "seismic_test_generator.h"


const seismic_source_driver_t g_seismic_source_test = {
    .name = "TEST",
    .transport_name = "SOFTWARE",
    .sample_rate_hz = SEISMIC_TEST_SOURCE_SAMPLE_RATE_HZ,
    .channel_count = SEISMIC_CHANNEL_COUNT,
    .core_id = APP_TASK_CORE_REALTIME,
    .start = seismic_test_generator_start,
    .stop = seismic_test_generator_stop,
    .populate_snapshot = seismic_source_snapshot_from_buffer,
};
