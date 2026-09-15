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


typedef enum
{
    SEISMIC_RECORDER_STOP_STAGE_NONE = 0,
    SEISMIC_RECORDER_STOP_STAGE_DRAIN,
    SEISMIC_RECORDER_STOP_STAGE_FLUSH,
    SEISMIC_RECORDER_STOP_STAGE_SYNC,
    SEISMIC_RECORDER_STOP_STAGE_CLOSE,
    SEISMIC_RECORDER_STOP_STAGE_METADATA,
    SEISMIC_RECORDER_STOP_STAGE_COMPLETE,
    SEISMIC_RECORDER_STOP_STAGE_ERROR

} seismic_recorder_stop_stage_t;


const char *seismic_recorder_stop_stage_name(
    seismic_recorder_stop_stage_t stage
);


typedef struct
{
    bool available;
    uint64_t timer_ticks;
    uint64_t conversion_attempts;
    uint64_t collapsed_periods;

} seismic_recorder_cadence_t;


typedef struct
{
    bool initialized;
    bool running;

    seismic_recorder_state_t state;
    seismic_recorder_stop_stage_t stop_stage;
    seismic_recorder_stop_stage_t stop_error_stage;

    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    seismic_recorder_cadence_t cadence;

    esp_err_t last_error;
    esp_err_t stop_stage_error;

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
    seismic_recorder_cadence_t cadence;

    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

} seismic_recorder_session_summary_t;


#define SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT  4U
#define SEISMIC_RECORDER_MAX_WAVEFORM_POINTS     1200U


typedef struct
{
    int64_t timestamp_us;
    int64_t end_timestamp_us;
    float min_uv[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
    float max_uv[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];

} seismic_recorder_waveform_point_t;


typedef struct
{
    bool complete;
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    seismic_recorder_cadence_t cadence;

    uint64_t source_frame_count;
    uint64_t start_frame;
    uint64_t frame_count;
    size_t point_count;
    int64_t start_timestamp_us;
    int64_t end_timestamp_us;

} seismic_recorder_waveform_info_t;


esp_err_t seismic_recorder_init(void);


esp_err_t seismic_recorder_start(
    const char *object_id
);


/* Request STOP and return after the stop target has been captured. */
esp_err_t seismic_recorder_stop(void);


/* Wait for an already-requested STOP to finish all SD and metadata I/O. */
esp_err_t seismic_recorder_wait_for_stop(void);


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
 * Cooperative status/Web listing. The parent directory census and every
 * session metadata read use separate storage arbitration windows so the TFT
 * can resume between records instead of being quiesced for a whole history.
 */
esp_err_t seismic_recorder_list_sessions_responsive(
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


/* Read one bounded session range as min/max envelopes for WebUI rendering. */
esp_err_t seismic_recorder_read_session(
    const char *object_id,
    const char *session_id,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
);
