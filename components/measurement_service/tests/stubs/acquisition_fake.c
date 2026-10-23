#include "seismic_acquisition.h"

#include <string.h>

esp_err_t measurement_source_start_result = ESP_OK;
unsigned measurement_source_ready_after;
unsigned measurement_source_snapshot_calls;
unsigned measurement_source_start_calls;
static bool s_running;

int64_t esp_timer_get_time(void) { return 1000000; }

esp_err_t seismic_acquisition_start(void)
{
    ++measurement_source_start_calls;
    s_running = measurement_source_start_result == ESP_OK;
    return measurement_source_start_result;
}

bool seismic_acquisition_is_running(void) { return s_running; }
void seismic_acquisition_poll(bool measurement_owns_source) { (void)measurement_owns_source; }

esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->running = s_running;
    snapshot->sample_rate_hz = 20000U;
    if (++measurement_source_snapshot_calls > measurement_source_ready_after) {
        snapshot->latest_valid = true;
        snapshot->samples = 1U;
        snapshot->last_sample_timestamp_us = esp_timer_get_time();
    }
    return ESP_OK;
}
