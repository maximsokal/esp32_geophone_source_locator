#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "seismic_recorder_start_policy.h"


int64_t esp_timer_get_time(void)
{
    return 1000000;
}


int main(void)
{
    seismic_acquisition_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.running = true;
    snapshot.source_healthy = false;
    snapshot.sample_rate_hz = 500U;
    snapshot.latest_valid = true;
    snapshot.samples = 1U;
    snapshot.last_sample_timestamp_us = 999000;

    /* The exact regression: stale BUSY presence must not reject START. */
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_OK);
    assert(
        seismic_recorder_start_validate_acquisition_at(&snapshot, 1000000) ==
        ESP_OK
    );

    snapshot.running = false;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.running = true;
    snapshot.sample_rate_hz = 0U;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.sample_rate_hz = 500U;
    snapshot.latest_valid = false;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.latest_valid = true;
    snapshot.samples = 0U;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.samples = 1U;
    snapshot.last_sample_timestamp_us = 1000000 - (2000 * 64) - 1;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.last_sample_timestamp_us = 1000001;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.last_sample_timestamp_us = 999000;
    assert(seismic_recorder_start_validate_acquisition_at(&snapshot, -1) == ESP_ERR_INVALID_STATE);
    assert(seismic_recorder_start_validate_acquisition(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_start_validate_acquisition_at(NULL, 1000000) == ESP_ERR_INVALID_ARG);
    return 0;
}
