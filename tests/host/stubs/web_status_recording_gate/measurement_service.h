#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_recorder.h"

typedef enum {
    MEASUREMENT_SERVICE_LIVE = 0,
    MEASUREMENT_SERVICE_STARTING,
    MEASUREMENT_SERVICE_RECORDING,
    MEASUREMENT_SERVICE_STOPPING,
    MEASUREMENT_SERVICE_REVIEW,
    MEASUREMENT_SERVICE_DELETING,
    MEASUREMENT_SERVICE_ERROR,
} measurement_service_state_t;

typedef struct {
    bool initialized;
    bool busy;
    bool active_object_present;
    measurement_service_state_t state;
    char object_id[16];
    char object_name[64];
    char session_id[16];
    seismic_recorder_stop_stage_t stop_stage;
    seismic_recorder_stop_stage_t stop_error_stage;
    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    uint32_t record_limit_ms;
    bool stopped_by_limit;
    esp_err_t last_error;
    esp_err_t stop_stage_error;
} measurement_service_snapshot_t;

esp_err_t measurement_service_get_snapshot(
    measurement_service_snapshot_t *snapshot
);
