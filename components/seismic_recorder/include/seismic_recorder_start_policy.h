#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "seismic_acquisition.h"


/*
 * Number of sample periods tolerated between the last successful sample and
 * START. This is deliberately derived from the configured cadence rather
 * than a transport-health or wall-clock constant.
 */
#define SEISMIC_RECORDER_START_FRESH_SAMPLE_PERIODS  64U

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
/* ESP-EMU recording scenario only: its synthetic source delivers a burst of frames every 10 ms FreeRTOS tick, the real
 * ADC one frame per period, so at 10 kHz and above the last frame is routinely older than 64 periods (6.4 ms). */
#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
#undef SEISMIC_RECORDER_START_FRESH_SAMPLE_PERIODS
#define SEISMIC_RECORDER_START_FRESH_SAMPLE_PERIODS  1000U
#endif


/*
 * Recorder admission depends on the acquisition task, sample cadence, and a
 * fresh successful sample. Physical source presence is reported separately
 * and is intentionally not an admission gate: BUSY telemetry may be stale
 * while samples are still arriving.
 */
esp_err_t seismic_recorder_start_validate_acquisition(
    const seismic_acquisition_snapshot_t *snapshot
);


/* Deterministic form used by host tests and callers with a sampled clock. */
esp_err_t seismic_recorder_start_validate_acquisition_at(
    const seismic_acquisition_snapshot_t *snapshot,
    int64_t now_us
);
