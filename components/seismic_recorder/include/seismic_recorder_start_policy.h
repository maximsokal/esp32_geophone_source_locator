#pragma once

#include "esp_err.h"
#include "seismic_acquisition.h"


/*
 * Recorder admission depends on the acquisition task and sample cadence.
 * Physical source presence is reported separately and is intentionally not
 * an admission gate: the acquisition task may continue while BUSY presence
 * telemetry is temporarily stale.
 */
esp_err_t seismic_recorder_start_validate_acquisition(
    const seismic_acquisition_snapshot_t *snapshot
);
