#include <assert.h>
#include <string.h>

#include "seismic_recorder_start_policy.h"


int main(void)
{
    seismic_acquisition_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.running = true;
    snapshot.source_healthy = false;
    snapshot.sample_rate_hz = 500U;

    /* The exact regression: stale BUSY presence must not reject START. */
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_OK);

    snapshot.running = false;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);

    snapshot.running = true;
    snapshot.sample_rate_hz = 0U;
    assert(seismic_recorder_start_validate_acquisition(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(seismic_recorder_start_validate_acquisition(NULL) == ESP_ERR_INVALID_ARG);
    return 0;
}
