#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define SEISMIC_RECORDER_OBJECT_ID_SIZE   16
#define SEISMIC_RECORDER_SESSION_ID_SIZE  16
#define SEISMIC_RECORDER_SOURCE_NAME_SIZE 24
#define SEISMIC_RECORDER_MAX_SESSIONS     32


typedef enum
{
    SEISMIC_RECORDER_IDLE = 0,
    SEISMIC_RECORDER_RECORDING,
    SEISMIC_RECORDER_STOPPING,
    SEISMIC_RECORDER_ERROR

} seismic_recorder_state_t;


typedef struct
{
    bool initialized;
    bool running;

    seismic_recorder_state_t state;

    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;

    esp_err_t last_error;

} seismic_recorder_snapshot_t;


typedef struct
{
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];

    bool complete;

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;

} seismic_recorder_session_summary_t;


esp_err_t seismic_recorder_init(void);


esp_err_t seismic_recorder_start(
    const char *object_id
);


esp_err_t seismic_recorder_stop(void);


esp_err_t seismic_recorder_get_snapshot(
    seismic_recorder_snapshot_t *snapshot
);


bool seismic_recorder_is_recording_object(
    const char *object_id
);


esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
);


/*
 * Delete one completed/stopped session directory from the active storage.
 * Deletion is intentionally rejected while any recording is running so a UI
 * review action cannot race the recorder's open FILE handles or metadata
 * finalization.
 */
esp_err_t seismic_recorder_delete_session(
    const char *object_id,
    const char *session_id
);
