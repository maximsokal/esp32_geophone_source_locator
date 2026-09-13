#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_buffer.h"


/*
 * Optional waveform index writer. samples.bin remains authoritative; every
 * caller must treat an error here as "disable/remove the acceleration index",
 * never as a reason to discard already-persisted seismic samples.
 */
esp_err_t seismic_recorder_lod_store_begin(
    const char *session_path,
    uint32_t sample_rate_hz
);


esp_err_t seismic_recorder_lod_store_append(
    const seismic_frame_t *persisted_frames,
    size_t frame_count
);


esp_err_t seismic_recorder_lod_store_flush(bool sync_to_media);


esp_err_t seismic_recorder_lod_store_close(void);


void seismic_recorder_lod_store_abort(void);


bool seismic_recorder_lod_store_is_active(void);
