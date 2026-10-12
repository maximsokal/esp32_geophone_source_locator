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
#define WORKFLOW_PLACEMENT_WINDOW_MS 3000U
#define WORKFLOW_PLACEMENT_MAX_RANGE_MM 50
#define WORKFLOW_PLACEMENT_MIN_SAMPLES 3U

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

typedef struct {
    char node[8];
    bool vertical;
    bool height_rel_main_mm_valid;
    int64_t height_rel_main_mm;
    bool altitude_asl_m_valid;
    double altitude_asl_m;
    bool stable;
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

typedef struct {
    workflow_step_t step;
    uint32_t generation;
    uint32_t allowed_actions;
    workflow_sensor_sufficiency_t sensor_sufficiency;
    uint8_t assigned_remotes;
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
