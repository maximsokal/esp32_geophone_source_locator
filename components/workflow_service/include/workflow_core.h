#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "object_store.h"
#include "seismic_recorder.h"

#define WORKFLOW_REASON_MAX 64U
#define WORKFLOW_PROGRESS_UNKNOWN (-1)
#define WORKFLOW_MAX_REMOTE_NODES 8U
#define WORKFLOW_MAX_CALIBRATION_NODES (WORKFLOW_MAX_REMOTE_NODES + 1U)
#define WORKFLOW_MAX_CALIBRATION_REASONS (WORKFLOW_MAX_REMOTE_NODES * 3U + 3U)
#define WORKFLOW_CALIBRATION_REASON_MAX 40U
/*
 * REMOTE sensor data arrives once per NRF_SENSOR_REFRESH_PERIOD_MS (2 s). The stability window must hold
 * WORKFLOW_PLACEMENT_MIN_SAMPLES of them at all times between refreshes, i.e. more than eight periods;
 * a 3 s window once kept two and the stability screen never completed (2026-10-31 bench).
 */
#define WORKFLOW_PLACEMENT_WINDOW_MS 20000U
/* A node whose newest sensor snapshot is older than this is not current (two refresh periods plus margin). */
#define WORKFLOW_PLACEMENT_FRESH_MS 5000U
/*
 * A confirmed setup is changed by a node that keeps showing MOVING for this long (frames at most FRESH_MS apart). The nRF
 * reports MOVING for 750 ms after a stop and after a pause between IMU samples (an I2C hiccup), so a single frame is not a move.
 */
#define WORKFLOW_CONFIRMED_MOTION_HOLD_MS 3000U
/*
 * The height of a node is the centre of the window, not the last sample: a barometer reports whole pascals
 * (about 83 mm each on the 12 Pa/m model), the MAIN reading wanders by a few pascals (bench log 2026-11-01
 * 18:16: 70..130 mm per node) and single readings are outliers. The centre is the mean of the samples left by a
 * Hampel filter (see workflow_placement_stats_t in workflow_service.c). A node is stable when that centre is
 * trustworthy to WORKFLOW_PLACEMENT_MAX_RANGE_MM (the operator's 0.1 m), judged by the larger of
 *   - the 95 % half-width of the centre (2 * sigma / sqrt(n)), and
 *   - how far the median of the newest WORKFLOW_PLACEMENT_RECENT_SAMPLES samples sits from the centre, scaled so
 *     that WORKFLOW_PLACEMENT_MAX_SHIFT_MM (the operator's 0.25 m) counts as the limit. At the 2 s cadence of the
 *     REMOTE nodes (at most 11 samples) the first term always binds earlier, so this is a backstop for a window
 *     that a faster source could fill.
 * The historic names "range" and "max_range_mm" are kept for the JSON and the screens; they now mean this bound.
 */
#define WORKFLOW_PLACEMENT_MAX_RANGE_MM 100
#define WORKFLOW_PLACEMENT_MAX_SHIFT_MM 250
/* The Hampel filter never trusts a spread below this: whole-pascal steps are about 83 mm apart. */
#define WORKFLOW_PLACEMENT_SIGMA_FLOOR_MM 40.0
#define WORKFLOW_PLACEMENT_MIN_SAMPLES 8U
/*
 * The window evicts by age, but the newest WORKFLOW_PLACEMENT_MIN_SAMPLES are kept while they are younger than this:
 * the 8 readings of a node that is still current (snapshot at most FRESH_MS old) can be FRESH_MS apart. Without it a
 * node refreshing slower than window / 7 gaps (about 2.9 s) never reached 8, and one at about 2.4 s flapped between
 * 7 and 9 (2026-11-05: 7/8 fell back to 5/8). At the nominal 2 s refresh the 20 s window binds first, as before.
 */
#define WORKFLOW_PLACEMENT_HARD_WINDOW_MS ((WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U) * WORKFLOW_PLACEMENT_FRESH_MS)
#define WORKFLOW_PLACEMENT_RECENT_SAMPLES 4U
/* Samples of one node are stored at most this often, so a 20 s window holds 21 of them at most. */
#define WORKFLOW_PLACEMENT_MIN_SAMPLE_GAP_MS 1000U
/*
 * MAIN's barometer misses readings now and then: a read times out while core 0 is busy and the sensor is re-armed
 * after a pause (bench log 2026-11-01 21:46: a miss every ~6 s for half a minute). Every miss used to wipe all
 * collected samples and the window had to start over, so the stability took 76 s instead of 16. While MAIN's
 * barometer is blind for at most this long the collected samples are kept and no new ones are taken.
 */
#define WORKFLOW_PLACEMENT_MAIN_BARO_GRACE_MS 15000U

typedef enum {
    WORKFLOW_STEP_IDLE = 0,
    WORKFLOW_STEP_OBJECT,
    WORKFLOW_STEP_SENSOR_CHECK,
    WORKFLOW_STEP_CALIB_PREP,
    WORKFLOW_STEP_CALIBRATING,
    WORKFLOW_STEP_CALIB_RESULT,
    WORKFLOW_STEP_PLACEMENT,
    WORKFLOW_STEP_GPS_CHOICE,
    WORKFLOW_STEP_GPS_PENDING,
    WORKFLOW_STEP_LAYOUT_READY,
    WORKFLOW_STEP_RANGING,
    WORKFLOW_STEP_RANGING_RESULT,
    WORKFLOW_STEP_SCAN_CHOICE,
    WORKFLOW_STEP_SCANNING,
    WORKFLOW_STEP_RECORD_READY,
    WORKFLOW_STEP_RECORDING,
    WORKFLOW_STEP_STOPPED,
    WORKFLOW_STEP_CALCULATING,
    WORKFLOW_STEP_RESULT,
    WORKFLOW_STEP_STOPPING,
    WORKFLOW_STEP_RECORD_ERROR,
    WORKFLOW_STEP_COUNT
} workflow_step_t;

typedef enum {
    WORKFLOW_COMMAND_START_MEASUREMENT = 0,
    WORKFLOW_COMMAND_GO_CALIBRATION,
    WORKFLOW_COMMAND_SENSORS_PLACED,
    WORKFLOW_COMMAND_CALIBRATION_DONE,
    WORKFLOW_COMMAND_RETRY,
    WORKFLOW_COMMAND_PLACE_SENSORS,
    WORKFLOW_COMMAND_PLACEMENT_CONFIRMED,
    WORKFLOW_COMMAND_REQUEST_GPS,
    WORKFLOW_COMMAND_GPS_ACCEPT,
    WORKFLOW_COMMAND_GPS_CAPTURE_FAILED,
    WORKFLOW_COMMAND_GPS_SKIP,
    WORKFLOW_COMMAND_CALCULATE_POSITIONS,
    WORKFLOW_COMMAND_RANGING_DONE,
    WORKFLOW_COMMAND_CONFIRM,
    WORKFLOW_COMMAND_START_RECORDING,
    WORKFLOW_COMMAND_STOP_RECORDING,
    WORKFLOW_COMMAND_MOVE_SENSORS,
    WORKFLOW_COMMAND_CALCULATE_SOURCES,
    WORKFLOW_COMMAND_CALCULATION_DONE,
    WORKFLOW_COMMAND_DELETE_RECORDING,
    WORKFLOW_COMMAND_RECORD_AGAIN,
    WORKFLOW_COMMAND_SCAN_START,
    WORKFLOW_COMMAND_SCAN_SKIP,
    WORKFLOW_COMMAND_SCAN_COMPLETE,
    WORKFLOW_COMMAND_BACK,
    WORKFLOW_COMMAND_SET_DISTANCE,
    /* Same as CALCULATE_POSITIONS, but the Wi-Fi radio stays on during the measurement (the user chose so on the page). */
    WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI,
    WORKFLOW_COMMAND_COUNT
} workflow_command_t;

typedef enum {
    WORKFLOW_GPS_STATUS_NONE = 0,
    WORKFLOW_GPS_STATUS_SKIPPED,
    WORKFLOW_GPS_STATUS_PENDING,
    WORKFLOW_GPS_STATUS_ACCEPTED,
    WORKFLOW_GPS_STATUS_FAILED
} workflow_gps_status_t;

typedef enum {
    WORKFLOW_SCAN_CHOICE_NONE = 0,
    WORKFLOW_SCAN_CHOICE_YES,
    WORKFLOW_SCAN_CHOICE_NO
} workflow_scan_choice_t;

typedef enum {
    WORKFLOW_SCAN_UPLOAD_NONE = 0,
    WORKFLOW_SCAN_UPLOAD_PENDING,
    WORKFLOW_SCAN_UPLOAD_VERIFIED,
    WORKFLOW_SCAN_UPLOAD_FAILED
} workflow_scan_upload_status_t;

typedef enum {
    WORKFLOW_SENSOR_INSUFFICIENT = 0,
    WORKFLOW_SENSOR_PLANAR_2D,
    WORKFLOW_SENSOR_CAPABLE_3D
} workflow_sensor_sufficiency_t;

typedef enum {
    WORKFLOW_CALCULATION_CLASS_NONE = 0,
    WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN,
    WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS,
    WORKFLOW_CALCULATION_CLASS_INTERNAL
} workflow_calculation_class_t;

typedef enum {
    WORKFLOW_COMMAND_RESULT_OK = 0,
    WORKFLOW_COMMAND_RESULT_NOT_ALLOWED,
    WORKFLOW_COMMAND_RESULT_STALE,
    WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT,
    WORKFLOW_COMMAND_RESULT_UNAVAILABLE
} workflow_command_result_t;

#define WORKFLOW_ACTION(command) (UINT32_C(1) << (unsigned)(command))

typedef struct {
    bool has_generation;
    uint32_t expected_generation;
    bool has_ok;
    bool ok;
    bool has_distance;
    uint8_t node_a, node_b;
    int32_t distance_mm;
    uint64_t ranging_started_at_ms, ranging_updated_at_ms;
    const char *reason;
    workflow_calculation_class_t calculation_class;
    bool has_location;
    double latitude;
    double longitude;
    double accuracy_m;
    bool altitude_valid;
    double altitude_m;
    double altitude_accuracy_m;
    uint64_t captured_at_ms;
} workflow_command_params_t;

typedef struct {
    char node[8];
    char height_state[24];
    bool relative_z_mm_valid;
    int32_t relative_z_mm;
    char imu_state[24];
    bool imu_values_valid;
    int16_t gyro_bias_mdps[3];
    int16_t gravity_ref_mg[3];
    bool cs_pairs_ok;
} workflow_calibration_node_t;

typedef struct {
    uint16_t generation;
    char phase[16];
    size_t per_node_count;
    workflow_calibration_node_t per_node[WORKFLOW_MAX_CALIBRATION_NODES];
    size_t pairs_done;
    size_t pairs_total;
    bool max_spread_valid;
    int32_t max_spread_mm;
    bool result_valid;
    bool result_ok;
    size_t reasons_count;
    char reasons[WORKFLOW_MAX_CALIBRATION_REASONS][WORKFLOW_CALIBRATION_REASON_MAX];
} workflow_calibration_snapshot_t;

/* Why a node is (not) stable; STABLE is also "no external blocker" where a blocker is passed in. */
typedef enum {
    WORKFLOW_PLACEMENT_REASON_STABLE = 0,
    WORKFLOW_PLACEMENT_REASON_COLLECTING,
    WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED,
    WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL,
    WORKFLOW_PLACEMENT_REASON_DEVICE_STATUS_BUSY,
    WORKFLOW_PLACEMENT_REASON_MAIN_REFERENCE,
    WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER,
    WORKFLOW_PLACEMENT_REASON_NODE_STALE,
    WORKFLOW_PLACEMENT_REASON_NODE_ENVIRONMENT,
    WORKFLOW_PLACEMENT_REASON_NODE_REFERENCE,
    WORKFLOW_PLACEMENT_REASON_SAMPLE_STALE,
    WORKFLOW_PLACEMENT_REASON_OVERFLOW_BLOCKED,
    WORKFLOW_PLACEMENT_REASON_CLOCK_BACKWARD,
    WORKFLOW_PLACEMENT_REASON_COUNT
} workflow_placement_reason_t;

typedef struct {
    char node[8];
    bool vertical;
    bool height_rel_main_mm_valid;
    int64_t height_rel_main_mm;     /* centre of the window once there are samples, else the latest reading */
    bool altitude_asl_m_valid;
    double altitude_asl_m;
    bool stable;
    workflow_placement_reason_t reason;
    uint8_t sample_count;
    int32_t range_mm;       /* uncertainty bound of the centre (see MAX_RANGE_MM); -1 until MIN_SAMPLES are collected */
} workflow_placement_node_t;

typedef struct {
    size_t per_node_count;
    workflow_placement_node_t per_node[WORKFLOW_MAX_CALIBRATION_NODES];
    bool all_stable;
} workflow_placement_snapshot_t;

typedef struct {
    uint32_t seq;
    workflow_command_t command;
    workflow_command_result_t result;
} workflow_last_async_t;

typedef struct {
    uint32_t seq;
    esp_err_t result;
} workflow_last_material_save_t;

/*
 * Were the channels independent sensors in the last calculation's analysis window? Channels are counted from 0. A
 * flat channel stayed silent while another moved; an identical pair carried one signal (same numbering as
 * localization_channel_health: 0:(0,1) 1:(0,2) 2:(0,3) 3:(1,2) 4:(1,3) 5:(2,3)).
 */
typedef struct {
    uint8_t flat_channel_mask;
    uint8_t identical_pair_mask;
} workflow_channel_health_t;

static inline bool workflow_channel_health_pair(unsigned pair, unsigned *channel_a, unsigned *channel_b)
{
    static const unsigned a[6] = {0U, 0U, 0U, 1U, 1U, 2U};
    static const unsigned b[6] = {1U, 2U, 3U, 2U, 3U, 3U};
    if (pair >= 6U) return false;
    if (channel_a != NULL) *channel_a = a[pair];
    if (channel_b != NULL) *channel_b = b[pair];
    return true;
}

typedef struct {
    workflow_step_t step;
    uint32_t generation;
    uint32_t allowed_actions;
    workflow_sensor_sufficiency_t sensor_sufficiency;
    uint8_t assigned_remotes;
    /* Assigned remotes that have not reported "level" (verticality ACCEPTED): a recording may start, but is diagnostic. */
    uint8_t unleveled_remotes;
    char reason[WORKFLOW_REASON_MAX];
    int progress;
    workflow_calculation_class_t calculation_class;
    char object_id[OBJECT_STORE_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
    bool has_scan;
    workflow_gps_status_t gps_status;
    bool layout_result_valid;
    bool layout_ok;
    workflow_scan_choice_t scan_choice;
    workflow_scan_upload_status_t scan_upload_status;
    bool calc_settings_ready;
    char calc_material[24];
    bool calc_velocity_range_set;
    float calc_velocity_range_min_mps;
    float calc_velocity_range_max_mps;
    workflow_last_async_t last_async;
    workflow_last_material_save_t last_material_save;
    workflow_command_t recommended_action;
    workflow_calibration_snapshot_t calibration;
    workflow_placement_snapshot_t placement;
    workflow_channel_health_t channel_health;
} workflow_snapshot_t;

typedef struct {
    workflow_step_t step;
    uint32_t generation;
    workflow_sensor_sufficiency_t sensor_sufficiency;
    uint8_t assigned_remotes;
    char reason[WORKFLOW_REASON_MAX];
    int progress;
    workflow_calculation_class_t calculation_class;
    bool calibration_ok;
    bool ranging_ok;
    bool ranging_keep_wifi;     /* the choice made on LAYOUT_READY: false = Wi-Fi goes off while the nodes range */
    bool calc_settings_ready;
    workflow_gps_status_t gps_status;
    bool layout_result_valid;
    bool layout_ok;
    workflow_scan_choice_t scan_choice;
    workflow_scan_upload_status_t scan_upload_status;
} workflow_core_t;

void workflow_core_init(workflow_core_t *core, bool active_object_present);
void workflow_core_reset_object(workflow_core_t *core, bool active_object_present, bool force_generation);
workflow_command_result_t workflow_core_apply(
    workflow_core_t *core,
    workflow_command_t command,
    const workflow_command_params_t *params,
    size_t assigned_remotes,
    bool placement_ready
);
void workflow_core_reconcile(
    workflow_core_t *core,
    workflow_step_t step,
    const char *reason
);
bool workflow_core_set_progress(workflow_core_t *core, int progress);
void workflow_core_set_calc_settings_ready(workflow_core_t *core, bool ready);
void workflow_core_snapshot(
    const workflow_core_t *core,
    size_t assigned_remotes,
    bool placement_ready,
    workflow_snapshot_t *snapshot
);

const char *workflow_core_step_name(workflow_step_t step);

/* Header-only so the web JSON writer and the firmware log share it without extra link dependencies. */
static inline const char *workflow_core_placement_reason_name(workflow_placement_reason_t reason)
{
    static const char *const names[WORKFLOW_PLACEMENT_REASON_COUNT] = {
        "stable", "collecting", "range_exceeded", "not_vertical", "device_status_busy",
        "main_reference", "main_barometer", "node_stale", "node_environment", "node_reference",
        "sample_stale", "overflow_blocked", "clock_backward"
    };
    return reason >= 0 && reason < WORKFLOW_PLACEMENT_REASON_COUNT ? names[reason] : "unknown";
}
const char *workflow_core_command_name(workflow_command_t command);
const char *workflow_core_sufficiency_name(workflow_sensor_sufficiency_t sufficiency);
const char *workflow_core_calculation_class_name(workflow_calculation_class_t calculation_class);
const char *workflow_core_gps_status_name(workflow_gps_status_t status);
const char *workflow_core_scan_choice_name(workflow_scan_choice_t choice);
const char *workflow_core_scan_upload_status_name(workflow_scan_upload_status_t status);
bool workflow_core_command_from_name(const char *name, workflow_command_t *command);
bool workflow_core_calculation_class_from_name(
    const char *name,
    workflow_calculation_class_t *calculation_class
);
