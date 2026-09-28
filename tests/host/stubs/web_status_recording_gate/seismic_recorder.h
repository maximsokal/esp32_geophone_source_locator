#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SEISMIC_RECORDER_SESSION_ID_SIZE 16U

typedef enum {
    SEISMIC_RECORDER_IDLE = 0,
    SEISMIC_RECORDER_RECORDING,
    SEISMIC_RECORDER_STOPPING,
    SEISMIC_RECORDER_ERROR,
} seismic_recorder_state_t;

typedef enum {
    SEISMIC_RECORDER_STOP_STAGE_NONE = 0,
    SEISMIC_RECORDER_STOP_STAGE_DRAIN,
    SEISMIC_RECORDER_STOP_STAGE_FLUSH,
    SEISMIC_RECORDER_STOP_STAGE_SYNC,
    SEISMIC_RECORDER_STOP_STAGE_CLOSE,
    SEISMIC_RECORDER_STOP_STAGE_METADATA,
    SEISMIC_RECORDER_STOP_STAGE_COMPLETE,
    SEISMIC_RECORDER_STOP_STAGE_ERROR,
} seismic_recorder_stop_stage_t;

typedef struct {
    bool initialized;
    bool running;
    seismic_recorder_state_t state;
    seismic_recorder_stop_stage_t stop_stage;
    seismic_recorder_stop_stage_t stop_error_stage;
    char object_id[16];
    char session_id[16];
    char source_name[24];
    uint32_t sample_rate_hz;
    uint8_t channel_count;
    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    esp_err_t last_error;
    esp_err_t stop_stage_error;
} seismic_recorder_snapshot_t;

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot);
const char *seismic_recorder_stop_stage_name(seismic_recorder_stop_stage_t stage);
