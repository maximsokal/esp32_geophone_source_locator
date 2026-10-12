#include "workflow_core.h"

#include <float.h>
#include <math.h>
#include <string.h>

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

static void workflow_core_set_reason(workflow_core_t *core, const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        core->reason[0] = '\0';
        return;
    }
    size_t length = 0U;
    while (length + 1U < sizeof(core->reason) && reason[length] != '\0') ++length;
    memcpy(core->reason, reason, length);
    core->reason[length] = '\0';
}

static void workflow_core_advance_generation(workflow_core_t *core)
{
    ++core->generation;
    if (core->generation == 0U) ++core->generation;
}

static void workflow_core_transition(
    workflow_core_t *core,
    workflow_step_t step,
    const char *reason
)
{
    if (core->step == step) {
        if (reason != NULL) workflow_core_set_reason(core, reason);
        return;
    }
    core->step = step;
    core->progress = WORKFLOW_PROGRESS_UNKNOWN;
    core->calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    workflow_core_set_reason(core, reason);
    workflow_core_advance_generation(core);
}

static workflow_sensor_sufficiency_t workflow_core_sufficiency(size_t assigned_remotes)
{
    if (assigned_remotes < 3U) return WORKFLOW_SENSOR_INSUFFICIENT;
    if (assigned_remotes == 3U) return WORKFLOW_SENSOR_PLANAR_2D;
    return WORKFLOW_SENSOR_CAPABLE_3D;
}

static uint32_t workflow_core_actions(
    workflow_step_t step,
    size_t assigned_remotes,
    bool placement_ready,
    bool calibration_ok,
    bool ranging_ok,
    workflow_calculation_class_t calculation_class,
    bool calc_settings_ready
)
{
    uint32_t actions = 0U;
#if !CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
    (void)assigned_remotes;
#endif
#define ADD_ACTION(command) (actions |= WORKFLOW_ACTION(command))
    switch (step) {
        case WORKFLOW_STEP_IDLE:
            break;
        case WORKFLOW_STEP_OBJECT:
            ADD_ACTION(WORKFLOW_COMMAND_START_MEASUREMENT);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_SENSOR_CHECK:
#if CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
            if (assigned_remotes >= 3U) ADD_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION);
#else
            ADD_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION);
#endif
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_CALIB_PREP:
            ADD_ACTION(WORKFLOW_COMMAND_SENSORS_PLACED);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_CALIBRATING:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_CALIB_RESULT:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            ADD_ACTION(calibration_ok
                ? WORKFLOW_COMMAND_PLACE_SENSORS
                : WORKFLOW_COMMAND_RETRY);
            break;
        case WORKFLOW_STEP_PLACEMENT:
            if (placement_ready) ADD_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_GPS_CHOICE:
            ADD_ACTION(WORKFLOW_COMMAND_REQUEST_GPS);
            ADD_ACTION(WORKFLOW_COMMAND_GPS_SKIP);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_GPS_PENDING:
            ADD_ACTION(WORKFLOW_COMMAND_GPS_SKIP);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_LAYOUT_READY:
            if (placement_ready) ADD_ACTION(WORKFLOW_COMMAND_CALCULATE_POSITIONS);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_RANGING:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_RANGING_RESULT:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            ADD_ACTION(ranging_ok ? WORKFLOW_COMMAND_CONFIRM : WORKFLOW_COMMAND_RETRY);
            break;
        case WORKFLOW_STEP_SCAN_CHOICE:
            ADD_ACTION(WORKFLOW_COMMAND_SCAN_START);
            ADD_ACTION(WORKFLOW_COMMAND_SCAN_SKIP);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_SCANNING:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_RECORD_READY:
            ADD_ACTION(WORKFLOW_COMMAND_START_RECORDING);
            ADD_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_RECORDING:
            ADD_ACTION(WORKFLOW_COMMAND_STOP_RECORDING);
            break;
        case WORKFLOW_STEP_STOPPED:
            if (calc_settings_ready) ADD_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES);
            ADD_ACTION(WORKFLOW_COMMAND_DELETE_RECORDING);
            ADD_ACTION(WORKFLOW_COMMAND_RECORD_AGAIN);
            ADD_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_CALCULATING:
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_RESULT:
            if (calculation_class != WORKFLOW_CALCULATION_CLASS_NONE) {
                ADD_ACTION(WORKFLOW_COMMAND_RECORD_AGAIN);
            }
            ADD_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS);
            ADD_ACTION(WORKFLOW_COMMAND_BACK);
            break;
        case WORKFLOW_STEP_COUNT:
        default:
            break;
    }
#undef ADD_ACTION
    return actions;
}

void workflow_core_init(workflow_core_t *core, bool active_object_present)
{
    if (core == NULL) return;
    memset(core, 0, sizeof(*core));
    core->step = active_object_present ? WORKFLOW_STEP_OBJECT : WORKFLOW_STEP_IDLE;
    core->generation = 1U;
    core->sensor_sufficiency = WORKFLOW_SENSOR_INSUFFICIENT;
    core->progress = WORKFLOW_PROGRESS_UNKNOWN;
}

void workflow_core_reset_object(
    workflow_core_t *core,
    bool active_object_present,
    bool force_generation
)
{
    if (core == NULL) return;
    const workflow_step_t step = active_object_present
        ? WORKFLOW_STEP_OBJECT
        : WORKFLOW_STEP_IDLE;
    if (core->step != step || force_generation) {
        core->step = step;
        core->progress = WORKFLOW_PROGRESS_UNKNOWN;
        core->calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
        core->calibration_ok = false;
        core->ranging_ok = false;
        core->calc_settings_ready = false;
        core->gps_status = WORKFLOW_GPS_STATUS_NONE;
        core->layout_result_valid = false;
        core->layout_ok = false;
        core->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
        core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
        workflow_core_set_reason(core, NULL);
        workflow_core_advance_generation(core);
    }
}

static workflow_step_t workflow_core_back_step(workflow_step_t step)
{
    /* BACK from CALIBRATING, RANGING, and CALCULATING cancels the running step. */
    switch (step) {
        case WORKFLOW_STEP_OBJECT: return WORKFLOW_STEP_COUNT;
        case WORKFLOW_STEP_SENSOR_CHECK: return WORKFLOW_STEP_OBJECT;
        case WORKFLOW_STEP_CALIB_PREP: return WORKFLOW_STEP_SENSOR_CHECK;
        case WORKFLOW_STEP_CALIBRATING: return WORKFLOW_STEP_CALIB_PREP;
        case WORKFLOW_STEP_CALIB_RESULT: return WORKFLOW_STEP_CALIB_PREP;
        case WORKFLOW_STEP_PLACEMENT: return WORKFLOW_STEP_CALIB_RESULT;
        case WORKFLOW_STEP_GPS_CHOICE: return WORKFLOW_STEP_PLACEMENT;
        case WORKFLOW_STEP_GPS_PENDING: return WORKFLOW_STEP_GPS_CHOICE;
        case WORKFLOW_STEP_LAYOUT_READY: return WORKFLOW_STEP_GPS_CHOICE;
        case WORKFLOW_STEP_RANGING: return WORKFLOW_STEP_LAYOUT_READY;
        case WORKFLOW_STEP_RANGING_RESULT: return WORKFLOW_STEP_LAYOUT_READY;
        case WORKFLOW_STEP_SCAN_CHOICE: return WORKFLOW_STEP_RANGING_RESULT;
        case WORKFLOW_STEP_SCANNING: return WORKFLOW_STEP_SCAN_CHOICE;
        case WORKFLOW_STEP_RECORD_READY: return WORKFLOW_STEP_SCAN_CHOICE;
        case WORKFLOW_STEP_STOPPED: return WORKFLOW_STEP_RECORD_READY;
        case WORKFLOW_STEP_CALCULATING: return WORKFLOW_STEP_STOPPED;
        case WORKFLOW_STEP_RESULT: return WORKFLOW_STEP_STOPPED;
        case WORKFLOW_STEP_IDLE:
        case WORKFLOW_STEP_RECORDING:
        case WORKFLOW_STEP_COUNT:
        default:
            return WORKFLOW_STEP_COUNT;
    }
}

static void workflow_core_clear_layout_result(workflow_core_t *core)
{
    core->layout_result_valid = false;
    core->layout_ok = false;
}

static void workflow_core_clear_scan_choice(workflow_core_t *core)
{
    core->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
    core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
}

static bool workflow_core_location_valid(const workflow_command_params_t *params)
{
    return params->has_location &&
        isfinite(params->latitude) && params->latitude >= -90.0 && params->latitude <= 90.0 &&
        isfinite(params->longitude) && params->longitude >= -180.0 && params->longitude <= 180.0 &&
        isfinite(params->accuracy_m) && params->accuracy_m >= 0.0 &&
        params->accuracy_m <= (double)FLT_MAX &&
        (!params->altitude_valid ||
            (isfinite(params->altitude_m) && isfinite(params->altitude_accuracy_m) &&
             params->altitude_accuracy_m >= 0.0 && params->altitude_accuracy_m <= (double)FLT_MAX));
}

workflow_command_result_t workflow_core_apply(
    workflow_core_t *core,
    workflow_command_t command,
    const workflow_command_params_t *params,
    size_t assigned_remotes,
    bool placement_ready
)
{
    if (core == NULL || params == NULL || command < 0 || command >= WORKFLOW_COMMAND_COUNT) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }
    core->assigned_remotes = assigned_remotes > UINT8_MAX ? UINT8_MAX : (uint8_t)assigned_remotes;
    core->sensor_sufficiency = workflow_core_sufficiency(assigned_remotes);
    if (params->has_generation && params->expected_generation != core->generation) {
        return WORKFLOW_COMMAND_RESULT_STALE;
    }

    workflow_step_t next = WORKFLOW_STEP_COUNT;
    const char *reason = NULL;
    switch (command) {
        case WORKFLOW_COMMAND_START_MEASUREMENT:
            if (core->step == WORKFLOW_STEP_OBJECT) next = WORKFLOW_STEP_SENSOR_CHECK;
            break;
        case WORKFLOW_COMMAND_GO_CALIBRATION:
            if (core->step == WORKFLOW_STEP_SENSOR_CHECK) {
#if CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
                if (assigned_remotes < 3U) {
                    workflow_core_set_reason(core, "insufficient_remotes");
                    return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
                }
#endif
                next = WORKFLOW_STEP_CALIB_PREP;
            }
            break;
        case WORKFLOW_COMMAND_SENSORS_PLACED:
            if (core->step == WORKFLOW_STEP_CALIB_PREP) next = WORKFLOW_STEP_CALIBRATING;
            break;
        case WORKFLOW_COMMAND_CALIBRATION_DONE:
            if (core->step == WORKFLOW_STEP_CALIBRATING && params->has_ok) {
                core->calibration_ok = params->ok;
                next = WORKFLOW_STEP_CALIB_RESULT;
                reason = params->ok ? NULL : (params->reason != NULL && params->reason[0] != '\0'
                    ? params->reason
                    : "calibration_failed");
            }
            else if (core->step == WORKFLOW_STEP_CALIBRATING) {
                return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
            }
            break;
        case WORKFLOW_COMMAND_RETRY:
            if (core->step == WORKFLOW_STEP_CALIB_RESULT && !core->calibration_ok) {
                next = WORKFLOW_STEP_CALIBRATING;
            }
            else if (core->step == WORKFLOW_STEP_RANGING_RESULT && !core->ranging_ok) {
                workflow_core_clear_layout_result(core);
                next = WORKFLOW_STEP_LAYOUT_READY;
            }
            break;
        case WORKFLOW_COMMAND_PLACE_SENSORS:
            if (core->step == WORKFLOW_STEP_CALIB_RESULT && core->calibration_ok) {
                core->gps_status = WORKFLOW_GPS_STATUS_NONE;
                workflow_core_clear_layout_result(core);
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_PLACEMENT;
            }
            break;
        case WORKFLOW_COMMAND_PLACEMENT_CONFIRMED:
            if (core->step == WORKFLOW_STEP_PLACEMENT) {
                if (!placement_ready) {
                    workflow_core_set_reason(core, "placement_not_ready");
                    return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
                }
                core->gps_status = WORKFLOW_GPS_STATUS_NONE;
                workflow_core_clear_layout_result(core);
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_GPS_CHOICE;
            }
            break;
        case WORKFLOW_COMMAND_REQUEST_GPS:
            if (core->step == WORKFLOW_STEP_GPS_CHOICE) {
                core->gps_status = WORKFLOW_GPS_STATUS_PENDING;
                next = WORKFLOW_STEP_GPS_PENDING;
            }
            break;
        case WORKFLOW_COMMAND_GPS_ACCEPT:
            if (core->step == WORKFLOW_STEP_GPS_PENDING) {
                if (!workflow_core_location_valid(params)) {
                    return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                }
                core->gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
                workflow_core_clear_layout_result(core);
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_LAYOUT_READY;
            }
            break;
        case WORKFLOW_COMMAND_GPS_CAPTURE_FAILED:
            if (core->step == WORKFLOW_STEP_GPS_PENDING) {
                core->gps_status = WORKFLOW_GPS_STATUS_FAILED;
                next = WORKFLOW_STEP_GPS_CHOICE;
                reason = params->reason != NULL && params->reason[0] != '\0'
                    ? params->reason
                    : "gps_capture_failed";
            }
            break;
        case WORKFLOW_COMMAND_GPS_SKIP:
            if (core->step == WORKFLOW_STEP_GPS_CHOICE ||
                core->step == WORKFLOW_STEP_GPS_PENDING) {
                core->gps_status = WORKFLOW_GPS_STATUS_SKIPPED;
                workflow_core_clear_layout_result(core);
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_LAYOUT_READY;
            }
            break;
        case WORKFLOW_COMMAND_CALCULATE_POSITIONS:
            if (core->step == WORKFLOW_STEP_LAYOUT_READY) {
                if (!placement_ready) {
                    workflow_core_set_reason(core, "placement_not_ready");
                    return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
                }
                workflow_core_clear_layout_result(core);
                next = WORKFLOW_STEP_RANGING;
            }
            break;
        case WORKFLOW_COMMAND_RANGING_DONE:
            if (core->step == WORKFLOW_STEP_RANGING && params->has_ok) {
                core->ranging_ok = params->ok;
                core->layout_result_valid = true;
                core->layout_ok = params->ok;
                next = WORKFLOW_STEP_RANGING_RESULT;
                reason = params->ok ? NULL : (params->reason != NULL && params->reason[0] != '\0'
                    ? params->reason
                    : "ranging_failed");
            }
            else if (core->step == WORKFLOW_STEP_RANGING) {
                return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
            }
            break;
        case WORKFLOW_COMMAND_CONFIRM:
            if (core->step == WORKFLOW_STEP_RANGING_RESULT && core->ranging_ok) {
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_SCAN_CHOICE;
            }
            break;
        case WORKFLOW_COMMAND_SCAN_START:
            if (core->step == WORKFLOW_STEP_SCAN_CHOICE) {
                core->scan_choice = WORKFLOW_SCAN_CHOICE_YES;
                core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_PENDING;
                next = WORKFLOW_STEP_SCANNING;
            }
            break;
        case WORKFLOW_COMMAND_SCAN_SKIP:
            if (core->step == WORKFLOW_STEP_SCAN_CHOICE) {
                core->scan_choice = WORKFLOW_SCAN_CHOICE_NO;
                core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
                next = WORKFLOW_STEP_RECORD_READY;
            }
            break;
        case WORKFLOW_COMMAND_SCAN_COMPLETE:
            if (core->step == WORKFLOW_STEP_SCANNING) {
                if (!params->has_ok || !params->ok) {
                    return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                }
                core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_VERIFIED;
                next = WORKFLOW_STEP_RECORD_READY;
            }
            break;
        case WORKFLOW_COMMAND_START_RECORDING:
            if (core->step == WORKFLOW_STEP_RECORD_READY) next = WORKFLOW_STEP_RECORDING;
            break;
        case WORKFLOW_COMMAND_STOP_RECORDING:
            if (core->step == WORKFLOW_STEP_RECORDING) next = WORKFLOW_STEP_STOPPED;
            break;
        case WORKFLOW_COMMAND_MOVE_SENSORS:
            if (core->step == WORKFLOW_STEP_RECORD_READY ||
                core->step == WORKFLOW_STEP_STOPPED ||
                core->step == WORKFLOW_STEP_RESULT) {
                core->gps_status = WORKFLOW_GPS_STATUS_NONE;
                workflow_core_clear_layout_result(core);
                workflow_core_clear_scan_choice(core);
                next = WORKFLOW_STEP_PLACEMENT;
            }
            break;
        case WORKFLOW_COMMAND_CALCULATE_SOURCES:
            if (core->step == WORKFLOW_STEP_STOPPED) {
                if (!core->calc_settings_ready) {
                    workflow_core_set_reason(core, "calc_settings_invalid");
                    return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
                }
                next = WORKFLOW_STEP_CALCULATING;
            }
            break;
        case WORKFLOW_COMMAND_CALCULATION_DONE:
            if (core->step == WORKFLOW_STEP_CALCULATING && params->has_ok) {
                if (!params->ok &&
                    params->calculation_class != WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN &&
                    params->calculation_class != WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS &&
                    params->calculation_class != WORKFLOW_CALCULATION_CLASS_INTERNAL) {
                    return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                }
                next = WORKFLOW_STEP_RESULT;
                core->calculation_class = params->ok
                    ? WORKFLOW_CALCULATION_CLASS_NONE
                    : params->calculation_class;
                reason = params->ok
                    ? NULL
                    : (params->reason != NULL && params->reason[0] != '\0'
                        ? params->reason
                        : workflow_core_calculation_class_name(params->calculation_class));
            }
            else if (core->step == WORKFLOW_STEP_CALCULATING) {
                return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
            }
            break;
        case WORKFLOW_COMMAND_DELETE_RECORDING:
            if (core->step == WORKFLOW_STEP_STOPPED) next = WORKFLOW_STEP_RECORD_READY;
            break;
        case WORKFLOW_COMMAND_RECORD_AGAIN:
            if (core->step == WORKFLOW_STEP_STOPPED ||
                (core->step == WORKFLOW_STEP_RESULT &&
                    core->calculation_class != WORKFLOW_CALCULATION_CLASS_NONE)) {
                next = WORKFLOW_STEP_RECORD_READY;
            }
            break;
        case WORKFLOW_COMMAND_BACK:
            if (core->step == WORKFLOW_STEP_CALIBRATING ||
                core->step == WORKFLOW_STEP_RANGING ||
                core->step == WORKFLOW_STEP_CALCULATING) {
                reason = "cancelled_by_back";
            }
            next = workflow_core_back_step(core->step);
            switch (core->step) {
                case WORKFLOW_STEP_GPS_CHOICE:
                case WORKFLOW_STEP_GPS_PENDING:
                case WORKFLOW_STEP_LAYOUT_READY:
                    core->gps_status = WORKFLOW_GPS_STATUS_NONE;
                    workflow_core_clear_layout_result(core);
                    workflow_core_clear_scan_choice(core);
                    break;
                case WORKFLOW_STEP_RANGING:
                case WORKFLOW_STEP_RANGING_RESULT:
                    workflow_core_clear_layout_result(core);
                    workflow_core_clear_scan_choice(core);
                    break;
                case WORKFLOW_STEP_SCAN_CHOICE:
                case WORKFLOW_STEP_SCANNING:
                case WORKFLOW_STEP_RECORD_READY:
                    workflow_core_clear_scan_choice(core);
                    break;
                default:
                    break;
            }
            break;
        case WORKFLOW_COMMAND_COUNT:
        default:
            return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }

    if (next == WORKFLOW_STEP_COUNT) return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    const uint32_t previous_generation = core->generation;
    workflow_core_transition(core, next, reason);
    if (core->generation == previous_generation) workflow_core_advance_generation(core);
    if (command == WORKFLOW_COMMAND_CALCULATION_DONE) {
        core->calculation_class = params->ok
            ? WORKFLOW_CALCULATION_CLASS_NONE
            : params->calculation_class;
    }
    return WORKFLOW_COMMAND_RESULT_OK;
}

void workflow_core_reconcile(
    workflow_core_t *core,
    workflow_step_t step,
    const char *reason
)
{
    if (core == NULL || step < 0 || step >= WORKFLOW_STEP_COUNT) return;
    workflow_core_transition(core, step, reason);
}

bool workflow_core_set_progress(workflow_core_t *core, int progress)
{
    if (core == NULL || core->step != WORKFLOW_STEP_CALCULATING ||
        (progress != WORKFLOW_PROGRESS_UNKNOWN && (progress < 0 || progress > 100))) {
        return false;
    }
    core->progress = progress;
    return true;
}

void workflow_core_set_calc_settings_ready(workflow_core_t *core, bool ready)
{
    if (core != NULL) core->calc_settings_ready = ready;
}

void workflow_core_snapshot(
    const workflow_core_t *core,
    size_t assigned_remotes,
    bool placement_ready,
    workflow_snapshot_t *snapshot
)
{
    if (core == NULL || snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->step = core->step;
    snapshot->generation = core->generation;
    snapshot->allowed_actions = 0U;
    snapshot->reason[0] = '\0';
    memcpy(snapshot->reason, core->reason, sizeof(snapshot->reason));
    snapshot->progress = core->progress;
    snapshot->calculation_class = core->calculation_class;
    snapshot->gps_status = core->gps_status;
    snapshot->layout_result_valid = core->layout_result_valid;
    snapshot->layout_ok = core->layout_ok;
    snapshot->scan_choice = core->scan_choice;
    snapshot->scan_upload_status = core->scan_upload_status;
    snapshot->calc_settings_ready = core->calc_settings_ready;
    snapshot->assigned_remotes = assigned_remotes > UINT8_MAX
        ? UINT8_MAX
        : (uint8_t)assigned_remotes;
    snapshot->sensor_sufficiency = workflow_core_sufficiency(assigned_remotes);
    snapshot->allowed_actions = workflow_core_actions(
        core->step,
        assigned_remotes,
        placement_ready,
        core->calibration_ok,
        core->ranging_ok,
        core->calculation_class,
        core->calc_settings_ready
    );
    snapshot->recommended_action = WORKFLOW_COMMAND_COUNT;
    if (core->step == WORKFLOW_STEP_RESULT) {
        if (core->calculation_class == WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN) {
            snapshot->recommended_action = WORKFLOW_COMMAND_RECORD_AGAIN;
        }
        else if (core->calculation_class == WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS) {
            snapshot->recommended_action = WORKFLOW_COMMAND_MOVE_SENSORS;
        }
    }
}

const char *workflow_core_step_name(workflow_step_t step)
{
    static const char *const names[WORKFLOW_STEP_COUNT] = {
        "IDLE", "OBJECT", "SENSOR_CHECK", "CALIB_PREP", "CALIBRATING",
        "CALIB_RESULT", "PLACEMENT", "GPS_CHOICE", "GPS_PENDING", "LAYOUT_READY",
        "RANGING", "RANGING_RESULT", "SCAN_CHOICE", "SCANNING", "RECORD_READY",
        "RECORDING", "STOPPED", "CALCULATING", "RESULT"
    };
    return step >= 0 && step < WORKFLOW_STEP_COUNT ? names[step] : "UNKNOWN";
}

const char *workflow_core_command_name(workflow_command_t command)
{
    static const char *const names[WORKFLOW_COMMAND_COUNT] = {
        "START_MEASUREMENT", "GO_CALIBRATION", "SENSORS_PLACED",
        "CALIBRATION_DONE", "RETRY", "PLACE_SENSORS", "PLACEMENT_CONFIRMED",
        "REQUEST_GPS", "GPS_ACCEPT", "GPS_CAPTURE_FAILED", "GPS_SKIP",
        "CALCULATE_POSITIONS", "RANGING_DONE", "CONFIRM", "START_RECORDING", "STOP_RECORDING",
        "MOVE_SENSORS", "CALCULATE_SOURCES", "CALCULATION_DONE",
        "DELETE_RECORDING", "RECORD_AGAIN", "SCAN_START", "SCAN_SKIP",
        "SCAN_COMPLETE", "BACK"
    };
    return command >= 0 && command < WORKFLOW_COMMAND_COUNT ? names[command] : "UNKNOWN";
}

const char *workflow_core_sufficiency_name(workflow_sensor_sufficiency_t sufficiency)
{
    switch (sufficiency) {
        case WORKFLOW_SENSOR_PLANAR_2D: return "PLANAR_2D";
        case WORKFLOW_SENSOR_CAPABLE_3D: return "CAPABLE_3D";
        case WORKFLOW_SENSOR_INSUFFICIENT:
        default: return "INSUFFICIENT";
    }
}

const char *workflow_core_calculation_class_name(workflow_calculation_class_t calculation_class)
{
    switch (calculation_class) {
        case WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN: return "record_again";
        case WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS: return "move_sensors";
        case WORKFLOW_CALCULATION_CLASS_INTERNAL: return "internal";
        case WORKFLOW_CALCULATION_CLASS_NONE:
        default: return "none";
    }
}

const char *workflow_core_gps_status_name(workflow_gps_status_t status)
{
    switch (status) {
        case WORKFLOW_GPS_STATUS_SKIPPED: return "skipped";
        case WORKFLOW_GPS_STATUS_PENDING: return "pending";
        case WORKFLOW_GPS_STATUS_ACCEPTED: return "accepted";
        case WORKFLOW_GPS_STATUS_FAILED: return "failed";
        case WORKFLOW_GPS_STATUS_NONE:
        default: return "none";
    }
}

const char *workflow_core_scan_choice_name(workflow_scan_choice_t choice)
{
    switch (choice) {
        case WORKFLOW_SCAN_CHOICE_YES: return "yes";
        case WORKFLOW_SCAN_CHOICE_NO: return "no";
        case WORKFLOW_SCAN_CHOICE_NONE:
        default: return "none";
    }
}

const char *workflow_core_scan_upload_status_name(workflow_scan_upload_status_t status)
{
    switch (status) {
        case WORKFLOW_SCAN_UPLOAD_PENDING: return "pending";
        case WORKFLOW_SCAN_UPLOAD_VERIFIED: return "verified";
        case WORKFLOW_SCAN_UPLOAD_FAILED: return "failed";
        case WORKFLOW_SCAN_UPLOAD_NONE:
        default: return "none";
    }
}

bool workflow_core_command_from_name(const char *name, workflow_command_t *command)
{
    if (name == NULL || command == NULL) return false;
    for (int candidate = 0; candidate < WORKFLOW_COMMAND_COUNT; ++candidate) {
        if (strcmp(name, workflow_core_command_name((workflow_command_t)candidate)) == 0) {
            *command = (workflow_command_t)candidate;
            return true;
        }
    }
    return false;
}

bool workflow_core_calculation_class_from_name(
    const char *name,
    workflow_calculation_class_t *calculation_class
)
{
    if (name == NULL || calculation_class == NULL) return false;
    if (strcmp(name, "record_again") == 0) {
        *calculation_class = WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN;
        return true;
    }
    if (strcmp(name, "move_sensors") == 0) {
        *calculation_class = WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS;
        return true;
    }
    if (strcmp(name, "internal") == 0) {
        *calculation_class = WORKFLOW_CALCULATION_CLASS_INTERNAL;
        return true;
    }
    if (strcmp(name, "none") == 0) {
        *calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
        return true;
    }
    return false;
}
