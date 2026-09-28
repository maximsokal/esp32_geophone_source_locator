#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_store.h"
#include "seismic_buffer.h"


#define SEISMIC_RECORDER_OBJECT_ID_SIZE   16
#define SEISMIC_RECORDER_SESSION_ID_SIZE  16
#define SEISMIC_RECORDER_SOURCE_NAME_SIZE 24
#define SEISMIC_RECORDER_MAX_SESSIONS     32
#define SEISMIC_RECORDER_HARDWARE_ID_MAX  8U


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


typedef enum
{
    SEISMIC_RECORDER_SESSION_UNKNOWN = 0,
    SEISMIC_RECORDER_SESSION_COMPLETE,
    SEISMIC_RECORDER_SESSION_ERROR,
    SEISMIC_RECORDER_SESSION_RECOVERED_ERROR

} seismic_recorder_session_status_t;


typedef bool (*seismic_recorder_recovery_stop_requested_fn)(
    void *user_context
);


typedef void (*seismic_recorder_recovery_request_callback_t)(
    const char *object_id,
    const char *session_id,
    void *user_context
);


const char *seismic_recorder_stop_stage_name(
    seismic_recorder_stop_stage_t stage
);


static inline const char *seismic_recorder_session_status_name(
    seismic_recorder_session_status_t status
)
{
    switch (status) {
        case SEISMIC_RECORDER_SESSION_COMPLETE:
            return "complete";
        case SEISMIC_RECORDER_SESSION_ERROR:
            return "error";
        case SEISMIC_RECORDER_SESSION_RECOVERED_ERROR:
            return "recovered_error";
        case SEISMIC_RECORDER_SESSION_UNKNOWN:
        default:
            return "unknown";
    }
}


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
    uint16_t format_version;

    esp_err_t last_error;
    esp_err_t stop_stage_error;

} seismic_recorder_snapshot_t;


typedef struct
{
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];

    seismic_recorder_session_status_t status;
    bool complete;
    bool readable;
    bool recovered;
    bool dropped_frames_available;
    bool recording_ms_available;

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    seismic_recorder_cadence_t cadence;
    uint16_t format_version;
    bool has_gaps;
    uint64_t missing_samples;

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


typedef enum
{
    SEISMIC_RECORDER_POSITION_SOURCE_NONE = 0,
    SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING,
    SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO,
} seismic_recorder_position_source_t;

typedef enum
{
    SEISMIC_RECORDER_GEOMETRY_Z_PLANAR = 0,
    SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
} seismic_recorder_geometry_z_policy_t;

#define SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE 40U
#define SEISMIC_RECORDER_SOLVER_NAME_SIZE 32U
#define SEISMIC_RECORDER_SOLVER_VERSION_SIZE 8U
#define SEISMIC_RECORDER_GEOMETRY_SETUP_ID_SIZE 64U
#define SEISMIC_RECORDER_RANGING_SOURCE_SIZE 24U

typedef struct
{
    bool available;
    seismic_recorder_geometry_z_policy_t effective_policy;
    char fallback_reason[SEISMIC_RECORDER_Z_FALLBACK_REASON_SIZE];
} seismic_recorder_geometry_z_provenance_t;


typedef struct
{
    bool available;
    char solver[SEISMIC_RECORDER_SOLVER_NAME_SIZE];
    char solver_version[SEISMIC_RECORDER_SOLVER_VERSION_SIZE];
    bool refined;
    uint8_t refine_iterations;
    uint32_t topology_generation;
    uint32_t cs_membership_generation;
    uint16_t main_height_reference_generation;
    char setup_id[SEISMIC_RECORDER_GEOMETRY_SETUP_ID_SIZE];
    uint64_t start_uptime_ms;
    char ranging_source[SEISMIC_RECORDER_RANGING_SOURCE_SIZE];
} seismic_recorder_geometry_provenance_t;


typedef struct
{
    bool assigned;
    bool node_id_known;
    uint8_t node_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[SEISMIC_RECORDER_HARDWARE_ID_MAX];
    seismic_recorder_position_source_t position_source;
    uint64_t source_sample_uptime_ms;
    uint64_t esp_receipt_uptime_ms;
    bool source_age_at_start_known;
    uint64_t source_age_ms_at_start;
    uint16_t height_reference_generation;
} seismic_recorder_pod_participation_t;


typedef struct
{
    seismic_recorder_pod_participation_t pods[OBJECT_STORE_POD_COUNT];
    seismic_recorder_geometry_z_provenance_t z_provenance;
    seismic_recorder_geometry_provenance_t geometry_provenance;
} seismic_recorder_session_participation_t;


typedef struct
{
    seismic_recorder_session_status_t status;
    bool complete;
    bool readable;
    bool recovered;
    bool dropped_frames_available;
    bool recording_ms_available;
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

    uint32_t sample_rate_hz;
    uint8_t channel_count;

    uint64_t samples_written;
    uint64_t dropped_frames;
    uint64_t recording_ms;
    seismic_recorder_cadence_t cadence;
    uint16_t format_version;
    bool has_gaps;
    uint64_t missing_samples;

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


esp_err_t seismic_recorder_set_recovery_request_callback(
    seismic_recorder_recovery_request_callback_t callback,
    void *user_context
);


/*
 * Cooperatively salvage stale recording sessions for one object. Only a
 * validated GEOSEIS1 whole-frame prefix is published, and the currently open
 * recorder session is never touched. recovered_count may be NULL.
 */
esp_err_t seismic_recorder_recover_object_sessions(
    const char *object_id,
    seismic_recorder_recovery_stop_requested_fn stop_requested,
    void *user_context,
    size_t *recovered_count
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


/* Read one bounded terminal session range as min/max envelopes for WebUI rendering. */
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


/*
 * MAX-168: read one entire finished session at full sample resolution, for
 * offline localization analysis rather than display. `summary_out` may be
 * NULL if the caller does not need session metadata (sample_rate_hz,
 * channel_count, etc.) alongside the frames.
 */
esp_err_t seismic_recorder_read_session_raw(
    const char *object_id,
    const char *session_id,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out
);


/*
 * MAX-168: read back the exact per-pod geometry MAX-166 froze into this
 * session at recording-start time -- never the object's current live
 * geometry, which a later recording of the same object may have already
 * overwritten. A pod stays `computed == false` if it was never solved for
 * this session, or if this session predates MAX-166 entirely.
 */
esp_err_t seismic_recorder_read_session_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry
);


esp_err_t seismic_recorder_read_session_participation(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_participation_t *participation
);


/*
 * MAX-148: read back the CS pair evidence frozen into this session
 * alongside its geometry -- never the object's current live topology. Stays
 * empty (pair_count == 0) if this session predates MAX-148 entirely.
 */
esp_err_t seismic_recorder_read_session_cs_pairs(
    const char *object_id,
    const char *session_id,
    object_store_cs_pairs_t *cs_pairs
);
