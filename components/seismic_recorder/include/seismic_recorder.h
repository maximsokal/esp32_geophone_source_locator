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
    uint64_t source_gap_samples;
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
    uint64_t ring_dropped_frames;
    uint64_t source_gap_samples;
    uint64_t recording_ms;
    seismic_recorder_cadence_t cadence;
    uint16_t format_version;
    bool has_gaps;
    uint64_t missing_samples;

    char source_name[SEISMIC_RECORDER_SOURCE_NAME_SIZE];

} seismic_recorder_session_summary_t;


typedef struct
{
    /* Number of v2 int16 samples examined in the requested range. */
    size_t samples_examined;
    /* Exact INT16_MIN or INT16_MAX code counts, one per recorder channel. */
    size_t rail_sample_counts[SEISMIC_CHANNEL_COUNT];

} seismic_recorder_raw_window_stats_t;


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
    /* REMOTE_ONLY: the centre of the geophone's AprilTag in the registered scan, taken as the sensor position (no offset). */
    SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX,
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
#define SEISMIC_RECORDER_TOPOLOGY_MODE_SIZE 16U

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
    int32_t range_offset_mm;
    bool range_offset_manual;
    /*
     * The topology the geometry was made on: "main_present" or "remote_only"; empty for a session recorded before this
     * field existed (read by the old scheme: a MAIN-origin geometry). With REMOTE_ONLY, `anchor_node_id` is the node at the
     * origin of the Channel Sounding layout (the lowest REMOTE), never node 0.
     */
    char topology_mode[SEISMIC_RECORDER_TOPOLOGY_MODE_SIZE];
    bool anchor_known;
    uint8_t anchor_node_id;
} seismic_recorder_geometry_provenance_t;


typedef struct
{
    bool assigned;
    bool node_id_known;
    uint8_t node_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[SEISMIC_RECORDER_HARDWARE_ID_MAX];
    seismic_recorder_position_source_t position_source;
    /* The AprilTag whose centre gave the position (APRILTAG_CENTER_APPROX only). */
    bool tag_id_known;
    uint8_t tag_id;
    uint64_t source_sample_uptime_ms;
    uint64_t esp_receipt_uptime_ms;
    bool source_age_at_start_known;
    uint64_t source_age_ms_at_start;
    uint16_t height_reference_generation;
} seismic_recorder_pod_participation_t;


typedef enum
{
    SEISMIC_RECORDER_LEVELING_UNKNOWN = 0,
    SEISMIC_RECORDER_LEVELING_NO_REMOTES,
    SEISMIC_RECORDER_LEVELING_INCOMPLETE,
    SEISMIC_RECORDER_LEVELING_ALL_ACCEPTED
} seismic_recorder_leveling_state_t;


typedef struct
{
    bool available;
    bool gate_bypassed;
    uint8_t assigned_remote_count;
    uint8_t accepted_remote_count;
    seismic_recorder_leveling_state_t leveling_state;
} seismic_recorder_gate_metadata_t;


#define SEISMIC_RECORDER_DIAGNOSTIC_MAX_EXCLUDED_PAIRS 10U

/*
 * What was doubtful when the recording started. It is shown to the operator and never blocks a recording: a confirmed
 * geometry that excludes some CS pairs, and an AD7606 whose serial stream looked suspicious or whose presence was stale.
 */
typedef struct
{
    bool available;
    bool partial_geometry;
    uint8_t excluded_pair_count;
    uint8_t excluded_pairs[SEISMIC_RECORDER_DIAGNOSTIC_MAX_EXCLUDED_PAIRS][2];
    bool source_suspicious;
    bool source_unhealthy;
    /* The AD7606 looked suspicious or its presence stale when the recording ended (read at STOP, not at START). */
    bool source_fault_at_stop;
} seismic_recorder_diagnostic_metadata_t;


typedef struct
{
    seismic_recorder_pod_participation_t pods[OBJECT_STORE_POD_COUNT];
    seismic_recorder_geometry_z_provenance_t z_provenance;
    seismic_recorder_geometry_provenance_t geometry_provenance;
    seismic_recorder_gate_metadata_t recording_gate;
    seismic_recorder_diagnostic_metadata_t diagnostic;
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

/* Read a session's frozen summary without opening its samples payload. */
esp_err_t seismic_recorder_get_session_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary_out
);

/*
 * Read at most max_frames logical frames from start_index. For v2,
 * source_sample_indices and gap_before retain source-index gaps; for v1,
 * indices are the session frame numbers and the stored timestamps are kept.
 * Optional metadata arrays may be NULL. start_index is a logical session
 * offset (0..samples_written); a value beyond the end is invalid.
 */
esp_err_t seismic_recorder_read_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out
);


/**
 * Read the same logical range as seismic_recorder_read_session_range() and,
 * while decoding v2 raw int16 codes, count exact rail samples per channel.
 * Source indexes and gap flags are returned from the same read. The stats
 * output is zeroed before reading; `samples_examined` counts v2 sample frames
 * only, so legacy float recordings report zero raw samples examined.
 *
 * `raw_stats_out` may be NULL when rail evidence is not needed.
 */
esp_err_t seismic_recorder_read_session_range_ex(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out,
    seismic_recorder_raw_window_stats_t *raw_stats_out
);

/* A callback sees one contiguous source-index run in the caller's buffer. */
typedef esp_err_t (*seismic_recorder_scan_callback_t)(
    const seismic_frame_t *frames,
    size_t count,
    uint64_t first_source_index,
    bool gap_before,
    void *context
);

/* Sequentially visit the complete session without rescanning earlier blocks.
 * Run from a worker task, not a UI callback. Callback errors stop the scan;
 * results are valid only if the scan succeeds. */
esp_err_t seismic_recorder_scan_session(
    const char *object_id,
    const char *session_id,
    seismic_frame_t *buffer,
    size_t capacity,
    seismic_recorder_scan_callback_t callback,
    void *context
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


#include "seismic_recorder_tag_layout.h"

/* ESP_ERR_NOT_FOUND: the session has no tag layout; ESP_ERR_INVALID_RESPONSE: the file breaks the contract (nothing is partially accepted). */
esp_err_t seismic_recorder_read_session_tag_layout(
    const char *object_id,
    const char *session_id,
    seismic_recorder_tag_layout_t *layout
);

#define SEISMIC_RECORDER_REGISTRATION_SCAN_ID_SIZE 32U

/*
 * The scan a session's registration-v1.json was made for (issue #31/#34): the session's own file, else the shared scan's. `scan_frame_id` is
 * empty for a legacy file without one (ESP_OK); ESP_ERR_NOT_FOUND when there is no registration; ESP_ERR_INVALID_RESPONSE for a malformed id.
 */
esp_err_t seismic_recorder_read_session_registration_scan_id(
    const char *object_id,
    const char *session_id,
    char scan_frame_id[SEISMIC_RECORDER_REGISTRATION_SCAN_ID_SIZE]
);

typedef enum
{
    SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING = 0,
    SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED,
    /* The tag layout of the session: MAIN-origin sensor frame, tagged remotes only, translated to the scan by `main_scan_mm`. */
    SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT,
} seismic_recorder_localization_geometry_source_t;

/*
 * The geometry a localization of this session works with: the tag layout when the phone saved one that covers enough remotes (see
 * seismic_recorder_geometry_apply_tag_layout), else the frozen Channel Sounding layout, or - for a session recorded in
 * REMOTE_ONLY whose registration carries valid positions made for exactly that layout - the registered AprilTag-centre
 * positions. One source for ALL participating nodes of a computation, never a mix; the stored Channel Sounding / barometer layout is untouched. Any reason the positions cannot be used leaves the Channel Sounding layout and is reported in
 * `registered_status` (may be NULL); only a failure to read the layout itself is an error. See
 * seismic_recorder_localization_geometry.h for the registration block.
 */
esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source,
    esp_err_t *registered_status
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
