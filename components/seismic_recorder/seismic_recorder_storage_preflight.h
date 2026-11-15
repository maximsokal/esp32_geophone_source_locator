#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


/*
 * START must have enough free space for at least one minute of the current raw
 * stream plus the configured write-behind cache. This is an admission runway,
 * not a promise that an arbitrarily long recording can never fill the card.
 */
#define SEISMIC_RECORDER_START_RUNWAY_SECONDS  60U

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
/* ESP-EMU recording scenario only: the flash FAT volume that stands in for the SD card holds ~6 MB, a minute of the
 * recording-rate sweep (up to 20 kHz x 24 bytes) would not fit. The board's SD card has gigabytes. */
#if defined(CONFIG_APP_TEST_RECORDING_SCENARIO) && CONFIG_APP_TEST_RECORDING_SCENARIO
#undef SEISMIC_RECORDER_START_RUNWAY_SECONDS
#define SEISMIC_RECORDER_START_RUNWAY_SECONDS  10U
#endif


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
