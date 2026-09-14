#pragma once

#include <stdint.h>

#include "esp_err.h"


/*
 * START must have enough free space for at least one minute of the current raw
 * stream plus the configured write-behind cache. This is an admission runway,
 * not a promise that an arbitrarily long recording can never fill the card.
 */
#define SEISMIC_RECORDER_START_RUNWAY_SECONDS  60U


uint64_t seismic_recorder_storage_required_bytes(
    uint32_t sample_rate_hz,
    size_t frame_size_bytes,
    uint64_t cache_reserve_bytes
);


/*
 * Query FATFS free space under storage-manager arbitration and reject START
 * before any new session directory/file is created when the runway is absent.
 */
esp_err_t seismic_recorder_storage_preflight(
    uint32_t sample_rate_hz
);
