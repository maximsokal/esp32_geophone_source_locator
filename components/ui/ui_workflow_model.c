#include "ui_workflow_model.h"
#include "ui_utf8_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define UI_WORKFLOW_ACTION_MATERIAL ((workflow_command_t)WORKFLOW_COMMAND_COUNT)
#define UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS 50U
#define UI_WORKFLOW_MATERIAL_RANGE_MAX_MPS 8000U

static const workflow_command_t s_action_order[] = {
    WORKFLOW_COMMAND_START_MEASUREMENT,
    WORKFLOW_COMMAND_GO_CALIBRATION,
    WORKFLOW_COMMAND_SENSORS_PLACED,
    WORKFLOW_COMMAND_RETRY,
    WORKFLOW_COMMAND_PLACE_SENSORS,
    WORKFLOW_COMMAND_PLACEMENT_CONFIRMED,
    WORKFLOW_COMMAND_REQUEST_GPS,
    WORKFLOW_COMMAND_GPS_SKIP,
    WORKFLOW_COMMAND_CALCULATE_POSITIONS,
    WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI,
    WORKFLOW_COMMAND_CONFIRM,
    WORKFLOW_COMMAND_START_RECORDING,
    WORKFLOW_COMMAND_STOP_RECORDING,
    WORKFLOW_COMMAND_MOVE_SENSORS,
    WORKFLOW_COMMAND_CALCULATE_SOURCES,
    WORKFLOW_COMMAND_DELETE_RECORDING,
    WORKFLOW_COMMAND_RECORD_AGAIN,
    WORKFLOW_COMMAND_SCAN_START,
    WORKFLOW_COMMAND_SCAN_SKIP,
    WORKFLOW_COMMAND_SET_DISTANCE,
    WORKFLOW_COMMAND_BACK,
};


void ui_workflow_remote_slot_layout(
    uint8_t connected_mask,
    ui_workflow_remote_slot_t slots[UI_WORKFLOW_REMOTE_SLOT_COUNT])
{
    if (slots == NULL) return;
    for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
        slots[i].visible = true;
        slots[i].connected = (connected_mask & (uint8_t)(1U << i)) != 0U;
    }
}

static void copy_arg(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0U) return;
    if (source == NULL) source = "";
    (void)ui_utf8_format(destination, capacity, "%s", source);
}

static ui_workflow_model_line_t *add_line(ui_workflow_model_t *model, ui_i18n_key_t key)
{
    if (model->line_count >= UI_WORKFLOW_MODEL_MAX_LINES) return NULL;
    ui_workflow_model_line_t *line = &model->lines[model->line_count++];
    memset(line, 0, sizeof(*line));
    line->key = key;
    for (size_t i = 0U; i < UI_WORKFLOW_MODEL_ARGS; ++i) {
        line->arg_keys[i] = UI_I18N_KEY_COUNT;
    }
    return line;
}

static void set_arg(ui_workflow_model_line_t *line, size_t index, const char *value)
{
    if (line == NULL || index >= UI_WORKFLOW_MODEL_ARGS) return;
    copy_arg(line->args[index], sizeof(line->args[index]), value);
}

static void set_key_arg(ui_workflow_model_line_t *line, size_t index, ui_i18n_key_t key)
{
    if (line == NULL || index >= UI_WORKFLOW_MODEL_ARGS) return;
    line->arg_keys[index] = key;
}

static ui_i18n_key_t action_key(workflow_command_t command)
{
    /* Not members of workflow_command_t, so they cannot be switch cases. */
    if (command == UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED) return UI_I18N_KEY_TFT_WORKFLOW_ACTION_PLACEMENT_CONFIRMED;
    if (command == UI_WORKFLOW_ACTION_LIVE_SIGNALS) return UI_I18N_KEY_TFT_WORKFLOW_ACTION_LIVE_SIGNALS;
    switch (command) {
        case WORKFLOW_COMMAND_START_MEASUREMENT: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_START_MEASUREMENT;
        case WORKFLOW_COMMAND_GO_CALIBRATION: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_GO_CALIBRATION;
        case WORKFLOW_COMMAND_SENSORS_PLACED: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_SENSORS_PLACED;
        case WORKFLOW_COMMAND_RETRY: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_RETRY;
        case WORKFLOW_COMMAND_PLACE_SENSORS: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_PLACE_SENSORS;
        case WORKFLOW_COMMAND_PLACEMENT_CONFIRMED: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_PLACEMENT_CONFIRMED;
        case WORKFLOW_COMMAND_REQUEST_GPS: return UI_I18N_KEY_TFT_WORKFLOW_GPS_WAITING;
        case WORKFLOW_COMMAND_GPS_SKIP:
        case WORKFLOW_COMMAND_SCAN_SKIP: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_SKIP;
        case WORKFLOW_COMMAND_SCAN_START: return UI_I18N_KEY_TFT_WORKFLOW_SCAN_PHONE;
        case WORKFLOW_COMMAND_CALCULATE_POSITIONS: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_CALCULATE_POSITIONS;
        case WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_CALCULATE_POSITIONS_KEEP_WIFI;
        case WORKFLOW_COMMAND_CONFIRM: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_CONFIRM;
        case WORKFLOW_COMMAND_SET_DISTANCE: return UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_EDIT;
        case WORKFLOW_COMMAND_START_RECORDING: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_START_RECORDING;
        case WORKFLOW_COMMAND_STOP_RECORDING: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_STOP_RECORDING;
        case WORKFLOW_COMMAND_MOVE_SENSORS: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_MOVE_SENSORS;
        case WORKFLOW_COMMAND_CALCULATE_SOURCES: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_CALCULATE_SOURCES;
        case WORKFLOW_COMMAND_DELETE_RECORDING: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_DELETE_RECORDING;
        case WORKFLOW_COMMAND_RECORD_AGAIN: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_RECORD_AGAIN;
        case WORKFLOW_COMMAND_BACK: return UI_I18N_KEY_TFT_WORKFLOW_ACTION_BACK;
        case WORKFLOW_COMMAND_COUNT: return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_ROW;
        case WORKFLOW_COMMAND_CALIBRATION_DONE:
        case WORKFLOW_COMMAND_GPS_ACCEPT:
        case WORKFLOW_COMMAND_GPS_CAPTURE_FAILED:
        case WORKFLOW_COMMAND_RANGING_DONE:
        case WORKFLOW_COMMAND_CALCULATION_DONE:
        case WORKFLOW_COMMAND_SCAN_COMPLETE:
        default:
            return UI_I18N_KEY_COUNT;
    }
}

/*
 * The highlighted action belongs to the step it was chosen in. It used to be carried over: while a
 * calibration runs BACK is the only action, the selection fell onto it, and on the result screen OK then
 * went BACK instead of forward to the placement step (2026-10-31 bench: "no button to continue").
 */
static void sync_selection_step(ui_workflow_model_state_t *state, workflow_step_t step)
{
    if (!state->selection_step_valid || state->selection_step != step) {
        state->has_selected_action = false;
        state->selection_step = step;
        state->selection_step_valid = true;
    }
}

/* The firmware hides "Sensors placed" until every node is stable; the screen still shows it, with the reason. */
static bool placement_continue_blocked(const workflow_snapshot_t *snapshot)
{
    return snapshot->step == WORKFLOW_STEP_PLACEMENT &&
        (snapshot->allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED)) == 0U;
}

static bool live_signals_offered(const workflow_snapshot_t *snapshot)
{
    /*
     * The operator order is: calibration, place the geophones, BLE Channel Sounding positions, and only then
     * the live signals (with the positions known). Before that the live view would show empty positions and
     * would load the AD7606 on top of the I2C work of calibration and ranging.
     */
    return snapshot->step == WORKFLOW_STEP_RANGING_RESULT;
}

/* What one s_action_order entry puts on the screen: the command, a stand-in for it, or extra entries before it. */
static size_t expand_action(
    const workflow_snapshot_t *snapshot,
    workflow_command_t command,
    workflow_command_t out[2])
{
    size_t count = 0U;
    if (command == WORKFLOW_COMMAND_BACK && live_signals_offered(snapshot)) {
        out[count++] = UI_WORKFLOW_ACTION_LIVE_SIGNALS;
    }
    if (command == WORKFLOW_COMMAND_PLACEMENT_CONFIRMED && placement_continue_blocked(snapshot)) {
        out[count++] = UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED;
    } else if ((snapshot->allowed_actions & WORKFLOW_ACTION(command)) != 0U) {
        out[count++] = command;
    }
    return count;
}

static size_t collect_actions(
    const workflow_snapshot_t *snapshot,
    const ui_workflow_model_state_t *state,
    workflow_command_t actions[UI_WORKFLOW_MODEL_MAX_ACTIONS])
{
    size_t count = 0U;
    if (snapshot->step == WORKFLOW_STEP_STOPPED && state->material_view == UI_WORKFLOW_MATERIAL_CLOSED) {
        actions[count++] = UI_WORKFLOW_ACTION_MATERIAL;
    }
    for (size_t i = 0U; i < sizeof(s_action_order) / sizeof(s_action_order[0]); ++i) {
        workflow_command_t expanded[2];
        const size_t expanded_count = expand_action(snapshot, s_action_order[i], expanded);
        for (size_t j = 0U; j < expanded_count; ++j) {
            if (action_key(expanded[j]) < UI_I18N_KEY_COUNT && count < UI_WORKFLOW_MODEL_MAX_ACTIONS) {
                actions[count++] = expanded[j];
            }
        }
    }
    return count;
}

static bool sequence_reached(uint32_t completed, uint32_t pending)
{
    /* shortcut: ordering is ambiguous at 2^31 IDs; revisit if requests can stay pending that long. */
    return (int32_t)(completed - pending) >= 0;
}

static void resolve_pending(ui_workflow_model_state_t *state, const workflow_snapshot_t *snapshot, uint64_t now_ms)
{
    if (state->has_pending && sequence_reached(snapshot->last_async.seq, state->pending_seq)) {
        state->has_pending = false;
        if (snapshot->last_async.result != WORKFLOW_COMMAND_RESULT_OK) {
            state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED;
            state->notice_until_ms = now_ms + 2000U;
        }
    }
    if (state->notice_key != UI_I18N_KEY_COUNT && now_ms >= state->notice_until_ms) {
        state->notice_key = UI_I18N_KEY_COUNT;
    }
}

static void resolve_material_save(
    ui_workflow_model_state_t *state,
    const workflow_snapshot_t *snapshot,
    uint64_t now_ms)
{
    if (!state->material_save_pending ||
        !sequence_reached(snapshot->last_material_save.seq, state->material_save_seq)) return;
    state->material_save_pending = false;
    if (snapshot->last_material_save.result == ESP_OK) {
        state->material_stored_index = state->material_selection_index;
        state->material_selection_initialized = true;
        state->material_view = UI_WORKFLOW_MATERIAL_CLOSED;
        state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVED;
    } else {
        state->material_view = UI_WORKFLOW_MATERIAL_SELECT;
        state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVE_FAILED;
    }
    state->notice_until_ms = now_ms + 2000U;
}

static void add_material_action(
    ui_workflow_model_t *model,
    const ui_workflow_model_state_t *state)
{
    if (model->action_count >= UI_WORKFLOW_MODEL_MAX_ACTIONS) return;
    const size_t index = model->action_count++;
    model->actions[index] = UI_WORKFLOW_ACTION_MATERIAL;
    for (size_t i = 0U; i < UI_WORKFLOW_MODEL_ARGS; ++i) {
        model->material_action_line.arg_keys[i] = UI_I18N_KEY_COUNT;
    }
    if (state->material_is_set) {
        model->material_action_line.key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_ROW;
        model->material_action_line.arg_keys[0] = state->material_name_key < UI_I18N_KEY_COUNT
            ? state->material_name_key : UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_UNKNOWN;
    } else {
        model->material_action_line.key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_UNSET;
    }
    model->action_keys[index] = model->material_action_line.key;
}

static void add_action(ui_workflow_model_t *model, workflow_command_t command)
{
    if (model->action_count >= UI_WORKFLOW_MODEL_MAX_ACTIONS) return;
    const ui_i18n_key_t key = action_key(command);
    if (key >= UI_I18N_KEY_COUNT) return;
    const size_t index = model->action_count++;
    model->actions[index] = command;
    model->action_keys[index] = key;
    if (command < WORKFLOW_COMMAND_COUNT) model->actions_mask |= WORKFLOW_ACTION(command);
}

static ui_i18n_key_t placement_reason_key(workflow_placement_reason_t reason)
{
    switch (reason) {
        case WORKFLOW_PLACEMENT_REASON_STABLE: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_STABLE;
        case WORKFLOW_PLACEMENT_REASON_COLLECTING: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_COLLECTING;
        case WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED;
        case WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL;
        case WORKFLOW_PLACEMENT_REASON_DEVICE_STATUS_BUSY: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_DEVICE_STATUS_BUSY;
        case WORKFLOW_PLACEMENT_REASON_MAIN_REFERENCE: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_MAIN_REFERENCE;
        case WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER;
        case WORKFLOW_PLACEMENT_REASON_NODE_STALE: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_NODE_STALE;
        case WORKFLOW_PLACEMENT_REASON_NODE_ENVIRONMENT: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_NODE_ENVIRONMENT;
        case WORKFLOW_PLACEMENT_REASON_NODE_REFERENCE: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_NODE_REFERENCE;
        case WORKFLOW_PLACEMENT_REASON_SAMPLE_STALE: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_SAMPLE_STALE;
        case WORKFLOW_PLACEMENT_REASON_OVERFLOW_BLOCKED: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_OVERFLOW_BLOCKED;
        case WORKFLOW_PLACEMENT_REASON_CLOCK_BACKWARD: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_CLOCK_BACKWARD;
        case WORKFLOW_PLACEMENT_REASON_COUNT:
        default: return UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_COLLECTING;
    }
}

static ui_i18n_key_t title_key(workflow_step_t step)
{
    switch (step) {
        case WORKFLOW_STEP_IDLE: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_IDLE;
        case WORKFLOW_STEP_OBJECT: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_OBJECT;
        case WORKFLOW_STEP_SENSOR_CHECK: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_SENSOR_CHECK;
        case WORKFLOW_STEP_CALIB_PREP: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIB_PREP;
        case WORKFLOW_STEP_CALIBRATING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIBRATING;
        case WORKFLOW_STEP_CALIB_RESULT: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIB_RESULT;
        case WORKFLOW_STEP_PLACEMENT: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_PLACEMENT;
        case WORKFLOW_STEP_GPS_CHOICE: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_GPS_CHOICE;
        case WORKFLOW_STEP_GPS_PENDING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_GPS_PENDING;
        case WORKFLOW_STEP_LAYOUT_READY: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_LAYOUT_READY;
        case WORKFLOW_STEP_RANGING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING;
        case WORKFLOW_STEP_RANGING_RESULT: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING_RESULT;
        case WORKFLOW_STEP_SCAN_CHOICE: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_SCAN_CHOICE;
        case WORKFLOW_STEP_SCANNING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_SCANNING;
        case WORKFLOW_STEP_RECORD_READY: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORD_READY;
        case WORKFLOW_STEP_RECORDING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORDING;
        case WORKFLOW_STEP_STOPPED: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_STOPPED;
        case WORKFLOW_STEP_CALCULATING: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALCULATING;
        case WORKFLOW_STEP_RESULT: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_RESULT;
        case WORKFLOW_STEP_COUNT:
        default: return UI_I18N_KEY_TFT_WORKFLOW_TITLE_IDLE;
    }
}

static ui_i18n_key_t command_result_key(bool ok)
{
    return ok ? UI_I18N_KEY_TFT_WORKFLOW_VALUE_OK : UI_I18N_KEY_TFT_WORKFLOW_VALUE_NO;
}

static ui_i18n_key_t result_status_key(bool ok)
{
    return ok ? UI_I18N_KEY_TFT_WORKFLOW_VALUE_OK : UI_I18N_KEY_TFT_WORKFLOW_VALUE_FAIL;
}

static ui_i18n_key_t state_value_key(const char *value)
{
    if (value == NULL || value[0] == '\0' || strcmp(value, "not_established") == 0 ||
        strcmp(value, "not_calibrated") == 0) {
        return UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
    }
    if (strcmp(value, "valid") == 0) return UI_I18N_KEY_TFT_WORKFLOW_STATE_VALID;
    if (strcmp(value, "calibrated") == 0) return UI_I18N_KEY_TFT_WORKFLOW_STATE_CALIBRATED;
    if (strcmp(value, "rejected_moving") == 0) return UI_I18N_KEY_TFT_WORKFLOW_STATE_MOVING;
    if (strcmp(value, "rejected_gravity") == 0) return UI_I18N_KEY_TFT_WORKFLOW_STATE_GRAVITY;
    if (strcmp(value, "sensor_invalid") == 0) return UI_I18N_KEY_TFT_WORKFLOW_STATE_SENSOR_INVALID;
    return UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
}

static ui_i18n_key_t phase_value_key(const char *value)
{
    if (value == NULL || value[0] == '\0') return UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
    if (strcmp(value, "idle") == 0) return UI_I18N_KEY_TFT_WORKFLOW_PHASE_IDLE;
    if (strcmp(value, "running") == 0) return UI_I18N_KEY_TFT_WORKFLOW_PHASE_RUNNING;
    if (strcmp(value, "complete") == 0) return UI_I18N_KEY_TFT_WORKFLOW_PHASE_COMPLETE;
    if (strcmp(value, "failed") == 0) return UI_I18N_KEY_TFT_WORKFLOW_PHASE_FAILED;
    if (strcmp(value, "cancelled") == 0) return UI_I18N_KEY_TFT_WORKFLOW_PHASE_CANCELLED;
    return UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
}

static ui_i18n_key_t calibration_reason_key(const char *reason)
{
    if (reason == NULL) return UI_I18N_KEY_COUNT;
    if (strcmp(reason, "insufficient_nodes") == 0) return UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT;
    if (strstr(reason, "height_rejected_moving") != NULL) return UI_I18N_KEY_TFT_WORKFLOW_REASON_HEIGHT_MOVING;
    if (strstr(reason, "height_sensor_invalid") != NULL || strstr(reason, "imu_sensor_invalid") != NULL) {
        return UI_I18N_KEY_TFT_WORKFLOW_REASON_SENSOR_INVALID;
    }
    if (strstr(reason, "imu_rejected_moving") != NULL) return UI_I18N_KEY_TFT_WORKFLOW_REASON_IMU_MOVING;
    if (strstr(reason, "imu_rejected_gravity") != NULL) return UI_I18N_KEY_TFT_WORKFLOW_REASON_IMU_GRAVITY;
    if (strstr(reason, "cs_pair_invalid") != NULL) return UI_I18N_KEY_TFT_WORKFLOW_REASON_CS_PAIR;
    if (strstr(reason, "cs_spread") != NULL) return UI_I18N_KEY_TFT_WORKFLOW_REASON_CS_SPREAD;
    if (strcmp(reason, "calibration_failed") == 0) return UI_I18N_KEY_TFT_WORKFLOW_REASON_FAILED;
    return UI_I18N_KEY_COUNT;
}

static void unknown_arg(ui_workflow_model_line_t *line, size_t index)
{
    set_key_arg(line, index, UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
}

static const workflow_placement_node_t *placement_blocker(const workflow_snapshot_t *snapshot)
{
    if (snapshot->placement.all_stable) return NULL;
    for (size_t i = 0U; i < snapshot->placement.per_node_count && i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
        if (!snapshot->placement.per_node[i].stable) return &snapshot->placement.per_node[i];
    }
    return NULL;
}

/*
 * Heights are shown in centimetres: the barometers cannot vouch for millimetres and the operator asks for
 * 0.1 m. A height is rounded to the nearest centimetre (halves away from zero); an uncertainty is rounded up,
 * so a bound that exceeds its limit never reads as equal to it ("11/10cm", not "10/10cm").
 */
static long long mm_to_cm_nearest(long long mm)
{
    return mm >= 0 ? (mm + 5) / 10 : -((-mm + 5) / 10);
}

static long mm_to_cm_up(long mm)
{
    return mm <= 0 ? 0 : (mm + 9) / 10;
}

/*
 * The first node that keeps the operator from continuing, why, and the number behind it: "G2: Not settled yet
 * 14/10cm" (uncertainty of the mean over the allowed one), "G1: Collecting 2/8" (the number carries its own
 * leading space). The node rows are all the screen has room for, so this goes first.
 */
static void add_placement_blocker_line(const workflow_placement_node_t *blocker, ui_workflow_model_t *model)
{
    ui_workflow_model_line_t *why = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_BLOCKED);
    set_arg(why, 0U, blocker->node);
    set_key_arg(why, 1U, placement_reason_key(blocker->reason));
    char detail[24] = "";
    if (blocker->reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED && blocker->range_mm >= 0) {
        (void)snprintf(detail, sizeof(detail), " %ld/%ldcm", mm_to_cm_up((long)blocker->range_mm),
            mm_to_cm_up((long)WORKFLOW_PLACEMENT_MAX_RANGE_MM));
    } else if (blocker->reason == WORKFLOW_PLACEMENT_REASON_COLLECTING) {
        (void)snprintf(detail, sizeof(detail), " %u/%u", (unsigned)blocker->sample_count,
            (unsigned)WORKFLOW_PLACEMENT_MIN_SAMPLES);
    }
    set_arg(why, 2U, detail);
}

static void add_reason(ui_workflow_model_t *model, const char *reason)
{
    if (reason == NULL || reason[0] == '\0') return;
    if (strcmp(reason, "insufficient_nodes") == 0) {
        (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT);
        return;
    }
    const char *separator = reason != NULL ? strchr(reason, ':') : NULL;
    const ui_i18n_key_t reason_key = calibration_reason_key(reason);
    if (separator != NULL && separator != reason && reason_key != UI_I18N_KEY_COUNT) {
        ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_REASON);
        if (line == NULL) return;
        const size_t prefix_length = (size_t)(separator - reason);
        const size_t copy_length = prefix_length < sizeof(line->args[0]) - 1U
            ? prefix_length : sizeof(line->args[0]) - 1U;
        const char *prefix = strncmp(reason, "cs_pair_invalid:", prefix_length) == 0
            ? separator + 1U : reason;
        const size_t displayed_prefix_length = prefix == reason ? copy_length : strlen(prefix);
        const size_t safe_length = displayed_prefix_length < sizeof(line->args[0]) - 1U
            ? displayed_prefix_length : sizeof(line->args[0]) - 1U;
        memcpy(line->args[0], prefix, safe_length);
        line->args[0][safe_length] = '\0';
        (void)ui_utf8_truncate(line->args[0], sizeof(line->args[0]));
        set_key_arg(line, 1U, reason_key);
    } else if (reason_key != UI_I18N_KEY_COUNT) {
        ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_REASON_SINGLE);
        set_key_arg(line, 0U, reason_key);
    } else {
        ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_REASON);
        set_arg(line, 0U, reason);
    }
}

static void build_result_lines(
    const workflow_snapshot_t *snapshot,
    const ui_workflow_model_state_t *state,
    ui_workflow_model_t *model)
{
    if (snapshot->calculation_class != WORKFLOW_CALCULATION_CLASS_NONE) {
        ui_i18n_key_t key = UI_I18N_KEY_TFT_WORKFLOW_RESULT_INTERNAL;
        if (snapshot->calculation_class == WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN) {
            key = UI_I18N_KEY_TFT_WORKFLOW_RESULT_RECORD_AGAIN;
        } else if (snapshot->calculation_class == WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS) {
            key = UI_I18N_KEY_TFT_WORKFLOW_RESULT_MOVE_SENSORS;
        }
        (void)add_line(model, key);
        if (snapshot->reason[0] != '\0') {
            ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_REASON);
            set_arg(line, 0U, snapshot->reason);
        }
        return;
    }

    (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_SUCCESS);
    if (!state->result_data_ready) {
        (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_LOADING);
        return;
    }
    if (state->result_load_failed) {
        (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_LOAD_FAILED);
    } else if (!state->result_waveform_available) {
        (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_WAVEFORM_UNAVAILABLE);
    }

    result_view_distance_t distances[OBJECT_STORE_POD_COUNT] = {0};
    if (state->result_source_valid && result_view_source_distances(
            state->result_source_m,
            state->result_geometry.pods,
            OBJECT_STORE_POD_COUNT,
            distances) != RESULT_VIEW_OK) memset(distances, 0, sizeof(distances));
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        char channel[8];
        (void)snprintf(channel, sizeof(channel), "%u", (unsigned)(i + 1U));
        ui_workflow_model_line_t *line = add_line(
            model,
            distances[i].known
                ? UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE
                : UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN);
        set_arg(line, 0U, channel);
        if (distances[i].known) {
            char distance[20];
            (void)snprintf(distance, sizeof(distance), "%.1f", distances[i].distance_m);
            set_arg(line, 1U, distance);
        }
    }
}

static bool step_can_preview(workflow_step_t step)
{
    return step == WORKFLOW_STEP_RECORD_READY || step == WORKFLOW_STEP_RECORDING ||
        step == WORKFLOW_STEP_RESULT;
}

static void add_material_speed_line(
    ui_workflow_model_t *model,
    uint32_t speed_mps,
    uint32_t sigma_mps)
{
    char speed[12], sigma[12];
    (void)snprintf(speed, sizeof(speed), "%lu", (unsigned long)speed_mps);
    (void)snprintf(sigma, sizeof(sigma), "%lu", (unsigned long)sigma_mps);
    ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SPEED);
    set_arg(line, 0U, speed);
    set_arg(line, 1U, sigma);
}

static uint32_t material_range_default_min(uint32_t speed_mps)
{
    const double units = (double)speed_mps / sqrt(10.0) / 50.0;
    const uint32_t rounded = (uint32_t)(floor(units + 0.5) * 50.0);
    return rounded < UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS
        ? UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS : rounded;
}

static uint32_t material_range_max_allowed(uint32_t min_mps)
{
    const uint32_t maximum = min_mps > 800U
        ? UI_WORKFLOW_MATERIAL_RANGE_MAX_MPS : min_mps * 10U;
    return (maximum / 50U) * 50U;
}

static void clamp_material_range(ui_workflow_model_state_t *state)
{
    if (state->material_custom_range_min_mps < UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS) {
        state->material_custom_range_min_mps = UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS;
    } else if (state->material_custom_range_min_mps > UI_WORKFLOW_MATERIAL_RANGE_MAX_MPS) {
        state->material_custom_range_min_mps = UI_WORKFLOW_MATERIAL_RANGE_MAX_MPS;
    }
    const uint32_t max_allowed = material_range_max_allowed(state->material_custom_range_min_mps);
    if (state->material_custom_range_max_mps < state->material_custom_range_min_mps) {
        state->material_custom_range_max_mps = state->material_custom_range_min_mps;
    } else if (state->material_custom_range_max_mps > max_allowed) {
        state->material_custom_range_max_mps = max_allowed;
    }
}

static void save_custom_material(
    ui_workflow_model_state_t *state,
    ui_workflow_event_t *event)
{
    state->material_selection_index = state->material_preset_count;
    state->material_view = UI_WORKFLOW_MATERIAL_SAVING;
    event->type = UI_WORKFLOW_EVENT_SAVE_MATERIAL;
    event->material_index = state->material_preset_count;
    event->custom_v_mps = state->material_custom_v_mps;
    event->custom_sigma_mps = state->material_custom_sigma_mps;
    event->has_range = state->material_custom_range_set;
    if (event->has_range) {
        event->range_min_mps = state->material_custom_range_min_mps;
        event->range_max_mps = state->material_custom_range_max_mps;
    }
}

static void build_material_picker(
    const ui_workflow_model_state_t *state,
    ui_workflow_model_t *model)
{
    model->title_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_TITLE;
    model->material_picker_visible = true;
    switch (state->material_view) {
        case UI_WORKFLOW_MATERIAL_SELECT: {
            ui_workflow_model_line_t *name = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_OPTION);
            set_key_arg(name, 0U, state->material_candidate_name_key);
            add_material_speed_line(model, state->material_candidate_v_mps, state->material_candidate_sigma_mps);
            model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_SELECT;
            break;
        }
        case UI_WORKFLOW_MATERIAL_CUSTOM_SPEED: {
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_CUSTOM_SPEED);
            char speed[12];
            (void)snprintf(speed, sizeof(speed), "%lu", (unsigned long)state->material_custom_v_mps);
            ui_workflow_model_line_t *value = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_CUSTOM_SPEED_VALUE);
            set_arg(value, 0U, speed);
            model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_ADJUST;
            break;
        }
        case UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA: {
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_CUSTOM_SIGMA);
            char sigma[12];
            (void)snprintf(sigma, sizeof(sigma), "%lu", (unsigned long)state->material_custom_sigma_mps);
            ui_workflow_model_line_t *value = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_CUSTOM_SIGMA_VALUE);
            set_arg(value, 0U, sigma);
            model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_SAVE;
            break;
        }
        case UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MIN: {
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_MIN);
            if (state->material_custom_range_set) {
                char min[12];
                (void)snprintf(min, sizeof(min), "%lu", (unsigned long)state->material_custom_range_min_mps);
                ui_workflow_model_line_t *value = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_VALUE);
                set_arg(value, 0U, min);
                model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_ADJUST;
            } else {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_AUTO);
                model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_HINT_AUTO;
            }
            break;
        }
        case UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MAX: {
            char max[12];
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_MAX);
            (void)snprintf(max, sizeof(max), "%lu", (unsigned long)state->material_custom_range_max_mps);
            ui_workflow_model_line_t *value = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_VALUE);
            set_arg(value, 0U, max);
            model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_SAVE;
            break;
        }
        case UI_WORKFLOW_MATERIAL_SAVING:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVING);
            model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_HINT_SELECT;
            break;
        case UI_WORKFLOW_MATERIAL_CLOSED:
        default:
            break;
    }
}

void ui_workflow_model_build(
    const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging,
    ui_workflow_model_state_t *state,
    uint64_t now_ms,
    ui_workflow_model_t *model)
{
    if (snapshot == NULL || state == NULL || model == NULL) return;
    memset(model, 0, sizeof(*model));
    model->title_key = title_key(snapshot->step);
    model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_HINT_NAVIGATION;
    model->notice_key = UI_I18N_KEY_COUNT;
    model->show_3d_hint = snapshot->has_scan && step_can_preview(snapshot->step);
    if (model->show_3d_hint) model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_HINT_3D;
    model->show_delete_confirm = state->delete_confirm;
    if (state->delete_confirm) model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_CONFIRM_DELETE;

    resolve_pending(state, snapshot, now_ms);
    resolve_material_save(state, snapshot, now_ms);
    if (state->distance_view == UI_WORKFLOW_DISTANCE_SAVING && !state->has_pending) {
        if (snapshot->last_async.result == WORKFLOW_COMMAND_RESULT_OK) {
            state->distance_view = UI_WORKFLOW_DISTANCE_CLOSED;
        } else {
            state->distance_view = UI_WORKFLOW_DISTANCE_VALUE;
            state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_REJECTED;
            state->notice_until_ms = now_ms + 4000U;
        }
    }
    if (state->distance_view != UI_WORKFLOW_DISTANCE_CLOSED &&
        state->distance_view != UI_WORKFLOW_DISTANCE_SAVING &&
        (snapshot->step != WORKFLOW_STEP_RANGING_RESULT || ranging == NULL || !ranging->available ||
         state->distance_generation != snapshot->generation ||
         state->distance_started_at_ms != ranging->started_at_ms ||
         state->distance_updated_at_ms != ranging->updated_at_ms)) {
        state->distance_view = UI_WORKFLOW_DISTANCE_CLOSED;
        state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED;
        state->notice_until_ms = now_ms + 2000U;
    }
    if (state->material_save_pending) {
        model->notice_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVING;
    } else if (state->notice_key != UI_I18N_KEY_COUNT && now_ms < state->notice_until_ms) {
        model->notice_key = state->notice_key;
    } else {
        state->notice_key = UI_I18N_KEY_COUNT;
    }

    if (state->material_view != UI_WORKFLOW_MATERIAL_CLOSED) {
        build_material_picker(state, model);
        return;
    }

    if (state->distance_view != UI_WORKFLOW_DISTANCE_CLOSED && ranging != NULL &&
        state->distance_pair < ranging->pair_count) {
        model->distance_picker_visible = true;
        model->title_key = UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_EDIT;
        model->hint_key = state->distance_view == UI_WORKFLOW_DISTANCE_PAIR
            ? UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_PAIR_HINT : UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_VALUE_HINT;
        char a[4], b[4], value[16], digit[12];
        (void)snprintf(a, sizeof(a), "%u", (unsigned)ranging->pairs[state->distance_pair].node_a);
        (void)snprintf(b, sizeof(b), "%u", (unsigned)ranging->pairs[state->distance_pair].node_b);
        ui_workflow_model_line_t *pair = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_PAIR);
        set_arg(pair, 0U, a); set_arg(pair, 1U, b);
        (void)snprintf(value, sizeof(value), "%06lu", (unsigned long)state->distance_mm);
        ui_workflow_model_line_t *distance = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_VALUE);
        set_arg(distance, 0U, value);
        if (state->distance_view == UI_WORKFLOW_DISTANCE_PAIR) {
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_CHOOSE);
        } else {
            (void)snprintf(digit, sizeof(digit), "%u", state->distance_digit + 1U);
            set_arg(add_line(model, UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_DIGIT), 0U, digit);
        }
        add_action(model, WORKFLOW_COMMAND_SET_DISTANCE);
        model->action_keys[0] = state->distance_view == UI_WORKFLOW_DISTANCE_PAIR
            ? UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_ENTER : UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_SAVE;
        add_action(model, WORKFLOW_COMMAND_BACK);
        if (state->distance_view == UI_WORKFLOW_DISTANCE_SAVING) {
            model->notice_key = UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_SAVING;
            model->pending = true;
        }
        return;
    }

    switch (snapshot->step) {
        case WORKFLOW_STEP_IDLE:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_IDLE_SELECT_OBJECT);
            break;
        case WORKFLOW_STEP_OBJECT: {
            ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_OBJECT_NAME);
            if (state->active_object_name[0] != '\0') set_arg(line, 0U, state->active_object_name);
            else unknown_arg(line, 0U);
            break;
        }
        case WORKFLOW_STEP_SENSOR_CHECK: {
            char remotes[12];
            (void)snprintf(remotes, sizeof(remotes), "%u", (unsigned)snapshot->assigned_remotes);
            ui_workflow_model_line_t *count = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_SENSOR_REMOTE_COUNT);
            set_arg(count, 0U, remotes);
            const ui_i18n_key_t sufficiency = snapshot->assigned_remotes < 3U
                ? UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT
                : snapshot->assigned_remotes == 3U
                    ? UI_I18N_KEY_TFT_WORKFLOW_SENSOR_PLANAR
                    : UI_I18N_KEY_TFT_WORKFLOW_SENSOR_READY;
            (void)add_line(model, sufficiency);
            break;
        }
        case WORKFLOW_STEP_CALIB_PREP:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_PREP);
            break;
        case WORKFLOW_STEP_CALIBRATING: {
            ui_workflow_model_line_t *header = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_HEADER);
            set_key_arg(header, 0U, phase_value_key(snapshot->calibration.phase));
            for (size_t i = 0U; i < snapshot->calibration.per_node_count && i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
                const workflow_calibration_node_t *node = &snapshot->calibration.per_node[i];
                char z[20], bias[20];
                if (node->relative_z_mm_valid) {
                    (void)snprintf(z, sizeof(z), "%lld", mm_to_cm_nearest((long long)node->relative_z_mm));
                }
                if (node->imu_values_valid) {
                    const double x = node->gyro_bias_mdps[0];
                    const double y = node->gyro_bias_mdps[1];
                    const double z_bias = node->gyro_bias_mdps[2];
                    (void)snprintf(bias, sizeof(bias), "%.0f", sqrt(x * x + y * y + z_bias * z_bias));
                }
                /* MAIN has no IMU: its row must not show IMU fields that can never fill in. */
                if (strcmp(node->imu_state, "not_applicable") == 0) {
                    ui_workflow_model_line_t *main_row =
                        add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_ROW_MAIN);
                    set_arg(main_row, 0U, node->node);
                    set_arg(main_row, 1U, node->height_state);
                    set_key_arg(main_row, 1U, state_value_key(node->height_state));
                    if (node->relative_z_mm_valid) set_arg(main_row, 2U, z);
                    else unknown_arg(main_row, 2U);
                    continue;
                }
                ui_workflow_model_line_t *row = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_ROW);
                set_arg(row, 0U, node->node);
                set_arg(row, 1U, node->height_state);
                set_key_arg(row, 1U, state_value_key(node->height_state));
                if (node->relative_z_mm_valid) set_arg(row, 2U, z);
                else unknown_arg(row, 2U);
                set_arg(row, 3U, node->imu_state);
                set_key_arg(row, 3U, state_value_key(node->imu_state));
                if (node->imu_values_valid) set_arg(row, 4U, bias);
                else unknown_arg(row, 4U);
            }
            break;
        }
        case WORKFLOW_STEP_CALIB_RESULT: {
            ui_workflow_model_line_t *status = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_STATUS);
            if (snapshot->calibration.result_valid) {
                set_key_arg(status, 0U, result_status_key(snapshot->calibration.result_ok));
            }
            else unknown_arg(status, 0U);
            for (size_t i = 0U; i < snapshot->calibration.reasons_count && i < WORKFLOW_MAX_CALIBRATION_REASONS; ++i) {
                add_reason(model, snapshot->calibration.reasons[i]);
            }
            /* A usable calibration leads to placing the geophones; the live signals come only after the positions. */
            if ((snapshot->allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACE_SENSORS)) != 0U) {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIB_RESULT_NEXT);
            }
            break;
        }
        case WORKFLOW_STEP_PLACEMENT:
            if (placement_blocker(snapshot) != NULL) add_placement_blocker_line(placement_blocker(snapshot), model);
            for (size_t i = 0U; i < snapshot->placement.per_node_count && i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
                const workflow_placement_node_t *node = &snapshot->placement.per_node[i];
                char height[24], altitude[24];
                if (node->height_rel_main_mm_valid) {
                    (void)snprintf(height, sizeof(height), "%lld", mm_to_cm_nearest((long long)node->height_rel_main_mm));
                }
                if (node->altitude_asl_m_valid) (void)snprintf(altitude, sizeof(altitude), "%.1f", node->altitude_asl_m);
                ui_workflow_model_line_t *row = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ROW);
                set_arg(row, 0U, node->node);
                set_key_arg(row, 1U, command_result_key(node->vertical));
                if (node->height_rel_main_mm_valid) set_arg(row, 2U, height);
                else unknown_arg(row, 2U);
                if (node->altitude_asl_m_valid) set_arg(row, 3U, altitude);
                else unknown_arg(row, 3U);
                set_key_arg(row, 4U, command_result_key(node->stable));
            }
            if (snapshot->placement.all_stable) (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ALL_STABLE);
            else if (placement_blocker(snapshot) == NULL) (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_WAITING);
            break;
        case WORKFLOW_STEP_GPS_CHOICE:
        case WORKFLOW_STEP_GPS_PENDING:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_GPS_WAITING);
            break;
        case WORKFLOW_STEP_LAYOUT_READY:
            (void)add_line(model, snapshot->placement.all_stable
                ? UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_READY
                : UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_WAITING);
            if (snapshot->placement.all_stable) {
                /* The choice is made by the two actions below: Wi-Fi off during the measurement, or kept on. */
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_WIFI_HINT);
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_WIFI_PHONE);
            }
            break;
        case WORKFLOW_STEP_RANGING: {
            char done[12], total[12], residual[20];
            if (ranging != NULL && ranging->radio_state == UI_WORKFLOW_RADIO_PENDING) {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_PENDING);
            } else if (ranging != NULL && ranging->radio_state == UI_WORKFLOW_RADIO_OFF) {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_OFF);
            }
            if (ranging != NULL && ranging->available) {
                (void)snprintf(done, sizeof(done), "%u", (unsigned)ranging->pairs_done);
                (void)snprintf(total, sizeof(total), "%u", (unsigned)ranging->pairs_total);
                if (ranging->residual_valid) (void)snprintf(residual, sizeof(residual), "%lld", mm_to_cm_nearest((long long)ranging->residual_mm));
                else residual[0] = '\0';
            } else {
                done[0] = '\0';
                total[0] = '\0';
                residual[0] = '\0';
            }
            ui_workflow_model_line_t *pairs = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIRS);
            if (ranging != NULL && ranging->available) {
                set_arg(pairs, 0U, done);
                set_arg(pairs, 1U, total);
                if (ranging->residual_valid) set_arg(pairs, 2U, residual);
                else unknown_arg(pairs, 2U);
            } else {
                unknown_arg(pairs, 0U);
                unknown_arg(pairs, 1U);
                unknown_arg(pairs, 2U);
            }
            if (ranging != NULL && ranging->available && ranging->pairs_total > 0U) {
                /* The nodes ranging on their own report which window of the plan is running. */
                const size_t done_clamped = ranging->pairs_done < ranging->pairs_total
                    ? ranging->pairs_done : ranging->pairs_total;
                char percent[8];
                (void)snprintf(percent, sizeof(percent), "%u",
                    (unsigned)((done_clamped * 100U) / ranging->pairs_total));
                set_arg(add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT_PERCENT), 0U, percent);
            } else {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT);
            }
            if (ranging != NULL && ranging->available) {
                for (size_t i = 0U; i < ranging->node_count && i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
                    ui_workflow_model_line_t *row = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_SIGMA);
                    set_arg(row, 0U, ranging->nodes[i]);
                    if (ranging->sigma_mm[i] == INT32_MAX) unknown_arg(row, 1U);
                    else {
                        char sigma[20];
                        (void)snprintf(sigma, sizeof(sigma), "%lld", mm_to_cm_nearest((long long)ranging->sigma_mm[i]));
                        set_arg(row, 1U, sigma);
                    }
                }
            }
            break;
        }
        case WORKFLOW_STEP_RANGING_RESULT: {
            if (ranging != NULL && ranging->available && ranging->pair_count > 0U)
                model->hint_key = UI_I18N_KEY_TFT_WORKFLOW_HINT_RANGING_PAGES;
            const bool success = (snapshot->allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM)) != 0U;
            const bool known = success || (snapshot->allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY)) != 0U;
            if (ranging != NULL && ranging->available && ranging->partial) {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_PARTIAL);
            } else if (known) {
                (void)add_line(model, success ? UI_I18N_KEY_TFT_WORKFLOW_RANGING_SUCCESS
                    : UI_I18N_KEY_TFT_WORKFLOW_RANGING_FAILED);
            } else {
                ui_workflow_model_line_t *status = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_STATUS);
                unknown_arg(status, 0U);
            }
            if (snapshot->reason[0] != '\0') {
                if (strcmp(snapshot->reason, "insufficient_nodes") == 0) {
                    (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT);
                    break;
                }
                ui_workflow_model_line_t *reason = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RESULT_REASON);
                set_arg(reason, 0U, snapshot->reason);
            }
            if (ranging != NULL && (ranging->radio_state == UI_WORKFLOW_RADIO_RESUMING ||
                                    ranging->radio_state == UI_WORKFLOW_RADIO_OFF)) {
                (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_BACK);
            }
            if (ranging != NULL && ranging->available) {
                char done[12], total[12], residual[20];
                (void)snprintf(done, sizeof(done), "%u", (unsigned)ranging->pairs_done);
                (void)snprintf(total, sizeof(total), "%u", (unsigned)ranging->pairs_total);
                ui_workflow_model_line_t *pairs = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIRS);
                set_arg(pairs, 0U, done);
                set_arg(pairs, 1U, total);
                if (ranging->residual_valid) {
                    (void)snprintf(residual, sizeof(residual), "%lld", mm_to_cm_nearest((long long)ranging->residual_mm));
                    set_arg(pairs, 2U, residual);
                } else {
                    unknown_arg(pairs, 2U);
                }
                for (size_t i = 0U; i < ranging->pair_count && i < UI_WORKFLOW_RANGING_MAX_PAIRS; ++i) {
                    char a[4], b[4], distance[16];
                    (void)snprintf(a, sizeof(a), "%u", (unsigned)ranging->pairs[i].node_a);
                    (void)snprintf(b, sizeof(b), "%u", (unsigned)ranging->pairs[i].node_b);
                    ui_workflow_model_line_t *row = add_line(model, ranging->pairs[i].manual
                        ? UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_MANUAL
                        : ranging->pairs[i].cable_exceeded
                        ? UI_I18N_KEY_TFT_WORKFLOW_RANGING_CABLE_EXCEEDED
                        : UI_I18N_KEY_TFT_WORKFLOW_RANGING_DISTANCE);
                    set_arg(row, 0U, a);
                    set_arg(row, 1U, b);
                    if (ranging->pairs[i].valid || ranging->pairs[i].cable_exceeded) {
                        (void)snprintf(distance, sizeof(distance), "%lld", mm_to_cm_nearest((long long)ranging->pairs[i].distance_mm));
                        set_arg(row, 2U, distance);
                    } else unknown_arg(row, 2U);
                }
                for (size_t i = 0U; i < ranging->node_count && i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
                    char sigma[20];
                    ui_workflow_model_line_t *row = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RANGING_SIGMA);
                    set_arg(row, 0U, ranging->nodes[i]);
                    if (ranging->sigma_mm[i] == INT32_MAX) unknown_arg(row, 1U);
                    else {
                        (void)snprintf(sigma, sizeof(sigma), "%lld", mm_to_cm_nearest((long long)ranging->sigma_mm[i]));
                        set_arg(row, 1U, sigma);
                    }
                }
            }
            break;
        }
        case WORKFLOW_STEP_SCAN_CHOICE:
        case WORKFLOW_STEP_SCANNING:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_SCAN_PHONE);
            break;
        case WORKFLOW_STEP_RECORD_READY:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RECORD_READY_INSTRUCTION);
            break;
        case WORKFLOW_STEP_RECORDING: {
            char elapsed[16];
            const uint64_t total_seconds = state->recording_elapsed_ms / 1000U;
            (void)snprintf(elapsed, sizeof(elapsed), "%02u:%02u",
                (unsigned)(total_seconds / 60U), (unsigned)(total_seconds % 60U));
            ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_RECORDING_ELAPSED);
            if (state->recording_elapsed_valid) set_arg(line, 0U, elapsed);
            else unknown_arg(line, 0U);
            break;
        }
        case WORKFLOW_STEP_STOPPED:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_STOPPED_INSTRUCTION);
            if (!snapshot->calc_settings_ready) (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_STOPPED_SETTINGS_MISSING);
            if (snapshot->calc_velocity_range_set) {
                char min[16], max[16];
                (void)snprintf(min, sizeof(min), "%.0f", (double)snapshot->calc_velocity_range_min_mps);
                (void)snprintf(max, sizeof(max), "%.0f", (double)snapshot->calc_velocity_range_max_mps);
                ui_workflow_model_line_t *range = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_RANGE_CURRENT);
                set_arg(range, 0U, min);
                set_arg(range, 1U, max);
            }
            break;
        case WORKFLOW_STEP_CALCULATING: {
            if (snapshot->progress < 0) {
                model->progress_indeterminate = true;
            } else {
                char progress[12];
                (void)snprintf(progress, sizeof(progress), "%u", (unsigned)snapshot->progress);
                ui_workflow_model_line_t *line = add_line(model, UI_I18N_KEY_TFT_WORKFLOW_CALCULATING_PROGRESS);
                set_arg(line, 0U, progress);
            }
            break;
        }
        case WORKFLOW_STEP_RESULT:
            build_result_lines(snapshot, state, model);
            break;
        case WORKFLOW_STEP_COUNT:
        default:
            (void)add_line(model, UI_I18N_KEY_TFT_WORKFLOW_IDLE_SELECT_OBJECT);
            break;
    }

    if (snapshot->step == WORKFLOW_STEP_STOPPED) add_material_action(model, state);
    for (size_t i = 0U; i < sizeof(s_action_order) / sizeof(s_action_order[0]); ++i) {
        workflow_command_t expanded[2];
        const size_t expanded_count = expand_action(snapshot, s_action_order[i], expanded);
        for (size_t j = 0U; j < expanded_count; ++j) add_action(model, expanded[j]);
    }
    if (state->has_pending) {
        model->pending = true;
        model->pending_command = state->pending_command;
    }
    sync_selection_step(state, snapshot->step);
    if (model->action_count == 0U) {
        model->selected_action = 0U;
        state->has_selected_action = false;
    } else {
        size_t selected = 0U;
        if (state->has_selected_action) {
            for (size_t i = 0U; i < model->action_count; ++i) {
                if (model->actions[i] == state->selected_action) {
                    selected = i;
                    break;
                }
            }
        }
        state->selected_action = model->actions[selected];
        state->has_selected_action = true;
        model->selected_action = selected;
    }
}

void ui_workflow_distance_open(const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging, ui_workflow_model_state_t *state)
{
    if (snapshot == NULL || ranging == NULL || state == NULL ||
        snapshot->step != WORKFLOW_STEP_RANGING_RESULT || !ranging->available ||
        ranging->pair_count == 0U || ranging->pair_count > UI_WORKFLOW_RANGING_MAX_PAIRS || state->has_pending) return;
    state->distance_pair = 0U;
    for (size_t i = 0U; i < ranging->pair_count; ++i) {
        if (!ranging->pairs[i].valid) { state->distance_pair = i; break; }
    }
    const int32_t value = ranging->pairs[state->distance_pair].distance_mm;
    state->distance_mm = value > 0 && value <= 999999 ? (uint32_t)value : 1000U;
    state->distance_digit = 0U;
    state->distance_generation = snapshot->generation;
    state->distance_started_at_ms = ranging->started_at_ms;
    state->distance_updated_at_ms = ranging->updated_at_ms;
    state->distance_view = UI_WORKFLOW_DISTANCE_PAIR;
}

ui_workflow_event_t ui_workflow_distance_handle_key(
    const workflow_snapshot_t *snapshot, const ui_workflow_ranging_t *ranging,
    ui_workflow_model_state_t *state, ui_workflow_key_t key, uint64_t now_ms)
{
    ui_workflow_event_t event = {.type = UI_WORKFLOW_EVENT_NONE};
    if (snapshot == NULL || ranging == NULL || state == NULL ||
        state->distance_view == UI_WORKFLOW_DISTANCE_CLOSED ||
        state->distance_view == UI_WORKFLOW_DISTANCE_SAVING) return event;
    if (snapshot->step != WORKFLOW_STEP_RANGING_RESULT || !ranging->available ||
        ranging->pair_count == 0U || ranging->pair_count > UI_WORKFLOW_RANGING_MAX_PAIRS ||
        state->distance_pair >= ranging->pair_count || state->distance_generation != snapshot->generation ||
        state->distance_started_at_ms != ranging->started_at_ms ||
        state->distance_updated_at_ms != ranging->updated_at_ms) {
        state->distance_view = UI_WORKFLOW_DISTANCE_CLOSED;
        return event;
    }
    if (state->distance_view == UI_WORKFLOW_DISTANCE_PAIR) {
        if (key == UI_WORKFLOW_KEY_LEFT) state->distance_view = UI_WORKFLOW_DISTANCE_CLOSED;
        else if (key == UI_WORKFLOW_KEY_OK) state->distance_view = UI_WORKFLOW_DISTANCE_VALUE;
        else if (key == UI_WORKFLOW_KEY_UP || key == UI_WORKFLOW_KEY_DOWN) {
            state->distance_pair = key == UI_WORKFLOW_KEY_DOWN
                ? (state->distance_pair + 1U) % ranging->pair_count
                : (state->distance_pair + ranging->pair_count - 1U) % ranging->pair_count;
            const int32_t value = ranging->pairs[state->distance_pair].distance_mm;
            state->distance_mm = value > 0 && value <= 999999 ? (uint32_t)value : 1000U;
        }
        return event;
    }
    static const uint32_t places[] = {100000U, 10000U, 1000U, 100U, 10U, 1U};
    const uint32_t place = places[state->distance_digit];
    if (key == UI_WORKFLOW_KEY_RIGHT) state->distance_digit = (state->distance_digit + 1U) % 6U;
    else if (key == UI_WORKFLOW_KEY_LEFT) {
        if (state->distance_digit == 0U) state->distance_view = UI_WORKFLOW_DISTANCE_PAIR;
        else --state->distance_digit;
    } else if (key == UI_WORKFLOW_KEY_UP || key == UI_WORKFLOW_KEY_DOWN) {
        const uint32_t digit = state->distance_mm / place % 10U;
        const uint32_t next = (digit + (key == UI_WORKFLOW_KEY_UP ? 1U : 9U)) % 10U;
        state->distance_mm = state->distance_mm - digit * place + next * place;
    } else if (key == UI_WORKFLOW_KEY_OK) {
        if (state->distance_mm == 0U) {
            state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_DISTANCE_REJECTED;
            state->notice_until_ms = now_ms + 4000U;
            return event;
        }
        event.type = UI_WORKFLOW_EVENT_SAVE_DISTANCE;
        event.command = WORKFLOW_COMMAND_SET_DISTANCE;
        event.distance = (workflow_command_params_t){
            .has_generation = true, .expected_generation = state->distance_generation,
            .has_distance = true, .node_a = ranging->pairs[state->distance_pair].node_a,
            .node_b = ranging->pairs[state->distance_pair].node_b, .distance_mm = (int32_t)state->distance_mm,
            .ranging_started_at_ms = state->distance_started_at_ms, .ranging_updated_at_ms = state->distance_updated_at_ms,
        };
        state->distance_view = UI_WORKFLOW_DISTANCE_SAVING;
    }
    return event;
}

ui_workflow_event_t ui_workflow_model_handle_key(
    const workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_key_t key,
    uint64_t now_ms)
{
    ui_workflow_event_t event = {.type = UI_WORKFLOW_EVENT_NONE};
    if (snapshot == NULL || state == NULL) return event;

    resolve_pending(state, snapshot, now_ms);
    resolve_material_save(state, snapshot, now_ms);
    if (state->material_view != UI_WORKFLOW_MATERIAL_CLOSED) {
        if (state->material_view == UI_WORKFLOW_MATERIAL_SAVING) return event;
        const size_t option_count = state->material_preset_count + 1U;
        switch (state->material_view) {
            case UI_WORKFLOW_MATERIAL_SELECT:
                if (key == UI_WORKFLOW_KEY_UP || key == UI_WORKFLOW_KEY_DOWN) {
                    state->material_selection_index = key == UI_WORKFLOW_KEY_DOWN
                        ? (state->material_selection_index + 1U) % option_count
                        : (state->material_selection_index + option_count - 1U) % option_count;
                } else if (key == UI_WORKFLOW_KEY_LEFT) {
                    state->material_view = UI_WORKFLOW_MATERIAL_CLOSED;
                } else if (key == UI_WORKFLOW_KEY_OK) {
                    state->notice_key = UI_I18N_KEY_COUNT;
                    state->notice_until_ms = 0U;
                    if (state->material_selection_index >= state->material_preset_count) {
                        if (!state->material_custom_values_valid) {
                            state->material_custom_v_mps = 1000U;
                            state->material_custom_sigma_mps = 200U;
                            state->material_custom_values_valid = true;
                        }
                        state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_SPEED;
                    } else {
                        event.type = UI_WORKFLOW_EVENT_SAVE_MATERIAL;
                        event.material_index = state->material_selection_index;
                        state->material_view = UI_WORKFLOW_MATERIAL_SAVING;
                    }
                }
                break;
            case UI_WORKFLOW_MATERIAL_CUSTOM_SPEED:
                if (key == UI_WORKFLOW_KEY_UP) {
                    state->material_custom_v_mps = state->material_custom_v_mps > 7900U
                        ? 7950U : state->material_custom_v_mps + 50U;
                } else if (key == UI_WORKFLOW_KEY_DOWN) {
                    state->material_custom_v_mps = state->material_custom_v_mps < 150U
                        ? 100U : state->material_custom_v_mps - 50U;
                } else if (key == UI_WORKFLOW_KEY_OK) {
                    state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA;
                } else if (key == UI_WORKFLOW_KEY_LEFT) {
                    state->material_view = UI_WORKFLOW_MATERIAL_SELECT;
                }
                if (key == UI_WORKFLOW_KEY_UP || key == UI_WORKFLOW_KEY_DOWN) {
                    if (state->material_custom_sigma_mps > state->material_custom_v_mps - 10U) {
                        state->material_custom_sigma_mps = state->material_custom_v_mps - 10U;
                    }
                    if (state->material_custom_sigma_mps < 10U) state->material_custom_sigma_mps = 10U;
                }
                break;
            case UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA: {
                const uint32_t maximum_sigma = state->material_custom_v_mps - 10U;
                if (key == UI_WORKFLOW_KEY_UP) {
                    state->material_custom_sigma_mps = state->material_custom_sigma_mps > maximum_sigma - 10U
                        ? maximum_sigma : state->material_custom_sigma_mps + 10U;
                } else if (key == UI_WORKFLOW_KEY_DOWN) {
                    state->material_custom_sigma_mps = state->material_custom_sigma_mps < 20U
                        ? 10U : state->material_custom_sigma_mps - 10U;
                } else if (key == UI_WORKFLOW_KEY_OK) {
                    state->material_custom_range_set = false;
                    state->material_custom_range_min_mps = material_range_default_min(state->material_custom_v_mps);
                    state->material_custom_range_max_mps = material_range_max_allowed(
                        state->material_custom_range_min_mps);
                    state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MIN;
                } else if (key == UI_WORKFLOW_KEY_LEFT) {
                    state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_SPEED;
                }
                break;
            }
            case UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MIN:
                clamp_material_range(state);
                if (key == UI_WORKFLOW_KEY_UP) {
                    if (!state->material_custom_range_set) {
                        state->material_custom_range_set = true;
                        state->material_custom_range_max_mps = material_range_max_allowed(
                            state->material_custom_range_min_mps);
                    } else if (state->material_custom_range_min_mps < UI_WORKFLOW_MATERIAL_RANGE_MAX_MPS) {
                        state->material_custom_range_min_mps += 50U;
                    }
                    clamp_material_range(state);
                } else if (key == UI_WORKFLOW_KEY_DOWN && state->material_custom_range_set) {
                    if (state->material_custom_range_min_mps > UI_WORKFLOW_MATERIAL_RANGE_MIN_MPS) {
                        state->material_custom_range_min_mps -= 50U;
                    }
                    clamp_material_range(state);
                } else if (key == UI_WORKFLOW_KEY_OK) {
                    if (state->material_custom_range_set) {
                        state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MAX;
                    } else {
                        save_custom_material(state, &event);
                    }
                } else if (key == UI_WORKFLOW_KEY_LEFT) {
                    state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA;
                }
                break;
            case UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MAX: {
                clamp_material_range(state);
                const uint32_t max_allowed = material_range_max_allowed(
                    state->material_custom_range_min_mps);
                if (key == UI_WORKFLOW_KEY_UP) {
                    state->material_custom_range_max_mps = state->material_custom_range_max_mps > max_allowed - 50U
                        ? max_allowed : state->material_custom_range_max_mps + 50U;
                } else if (key == UI_WORKFLOW_KEY_DOWN) {
                    state->material_custom_range_max_mps = state->material_custom_range_max_mps <
                        state->material_custom_range_min_mps + 50U
                            ? state->material_custom_range_min_mps
                            : state->material_custom_range_max_mps - 50U;
                } else if (key == UI_WORKFLOW_KEY_OK) {
                    save_custom_material(state, &event);
                } else if (key == UI_WORKFLOW_KEY_LEFT) {
                    state->material_view = UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MIN;
                }
                clamp_material_range(state);
                break;
            }
            case UI_WORKFLOW_MATERIAL_SAVING:
            case UI_WORKFLOW_MATERIAL_CLOSED:
            default:
                break;
        }
        return event;
    }
    if (key == UI_WORKFLOW_KEY_OK && state->has_pending) return event;
    sync_selection_step(state, snapshot->step);
    switch (key) {
        case UI_WORKFLOW_KEY_UP:
        case UI_WORKFLOW_KEY_DOWN: {
            state->delete_confirm = false;
            workflow_command_t actions[UI_WORKFLOW_MODEL_MAX_ACTIONS];
            const size_t action_count = collect_actions(snapshot, state, actions);
            if (action_count > 0U) {
                size_t selected = 0U;
                if (state->has_selected_action) {
                    for (size_t i = 0U; i < action_count; ++i) {
                        if (actions[i] == state->selected_action) {
                            selected = i;
                            break;
                        }
                    }
                }
                selected = key == UI_WORKFLOW_KEY_DOWN
                    ? (selected + 1U) % action_count
                    : (selected + action_count - 1U) % action_count;
                state->selected_action = actions[selected];
                state->has_selected_action = true;
            }
            break;
        }
        case UI_WORKFLOW_KEY_LEFT:
            if (state->delete_confirm) {
                state->delete_confirm = false;
                event.type = UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED;
            } else if (snapshot->step == WORKFLOW_STEP_OBJECT) {
                event.type = UI_WORKFLOW_EVENT_LEAVE_HOME;
            } else if ((snapshot->allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK)) != 0U) {
                event.type = UI_WORKFLOW_EVENT_SEND_COMMAND;
                event.command = WORKFLOW_COMMAND_BACK;
            } else if (snapshot->step == WORKFLOW_STEP_IDLE || snapshot->step == WORKFLOW_STEP_OBJECT ||
                snapshot->step == WORKFLOW_STEP_SENSOR_CHECK) {
                event.type = UI_WORKFLOW_EVENT_LEAVE_OBJECTS;
            }
            break;
        case UI_WORKFLOW_KEY_RIGHT:
            if (snapshot->has_scan && step_can_preview(snapshot->step)) event.type = UI_WORKFLOW_EVENT_OPEN_PREVIEW;
            break;
        case UI_WORKFLOW_KEY_OK:
        {
            workflow_command_t actions[UI_WORKFLOW_MODEL_MAX_ACTIONS];
            const size_t action_count = collect_actions(snapshot, state, actions);
            if (action_count == 0U) break;
            size_t selected = 0U;
            if (state->has_selected_action) {
                for (size_t i = 0U; i < action_count; ++i) {
                    if (actions[i] == state->selected_action) {
                        selected = i;
                        break;
                    }
                }
            }
            event.command = actions[selected];
            if (event.command == WORKFLOW_COMMAND_SET_DISTANCE) {
                event.type = UI_WORKFLOW_EVENT_EDIT_DISTANCE;
            } else if (event.command == UI_WORKFLOW_ACTION_MATERIAL) {
                state->material_selection_index = state->material_stored_index <= state->material_preset_count
                    ? state->material_stored_index : 0U;
                state->material_view = UI_WORKFLOW_MATERIAL_SELECT;
                state->notice_key = UI_I18N_KEY_COUNT;
                state->notice_until_ms = 0U;
            } else if (event.command == UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED) {
                /* Not a command: the firmware would refuse it. Explain instead; the reason is on screen. */
                event.type = UI_WORKFLOW_EVENT_NONE;
                state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_NOTICE_PLACEMENT_BLOCKED;
                state->notice_until_ms = now_ms + 2000U;
            } else if (event.command == UI_WORKFLOW_ACTION_LIVE_SIGNALS) {
                event.type = UI_WORKFLOW_EVENT_OPEN_LIVE_SIGNALS;
            } else if (event.command == WORKFLOW_COMMAND_DELETE_RECORDING && !state->delete_confirm) {
                state->delete_confirm = true;
                event.type = UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED;
            } else if (event.command == WORKFLOW_COMMAND_DELETE_RECORDING) {
                state->delete_confirm = false;
                event.type = UI_WORKFLOW_EVENT_SEND_COMMAND;
            } else {
                event.type = UI_WORKFLOW_EVENT_SEND_COMMAND;
            }
            break;
        }
        default:
            break;
    }
    return event;
}

void ui_workflow_model_command_sent(
    ui_workflow_model_state_t *state,
    workflow_command_t command,
    esp_err_t result,
    uint32_t seq,
    uint64_t now_ms)
{
    if (state == NULL) return;
    if (result == ESP_OK) {
        state->has_pending = true;
        state->pending_command = command;
        state->pending_seq = seq;
        state->notice_key = UI_I18N_KEY_COUNT;
        state->notice_until_ms = 0U;
    } else {
        state->has_pending = false;
        state->notice_key = result == ESP_ERR_NO_MEM
            ? UI_I18N_KEY_TFT_WORKFLOW_NOTICE_BUSY
            : UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED;
        state->notice_until_ms = now_ms + 2000U;
    }
}

void ui_workflow_model_material_save_sent(
    ui_workflow_model_state_t *state,
    esp_err_t result,
    uint32_t seq,
    uint64_t now_ms)
{
    if (state == NULL) return;
    if (result == ESP_OK) {
        state->material_save_pending = true;
        state->material_save_seq = seq;
        state->material_view = UI_WORKFLOW_MATERIAL_SAVING;
        state->notice_key = UI_I18N_KEY_COUNT;
        state->notice_until_ms = 0U;
    } else {
        state->material_save_pending = false;
        state->material_view = UI_WORKFLOW_MATERIAL_SELECT;
        state->notice_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVE_FAILED;
        state->notice_until_ms = now_ms + 2000U;
    }
}

bool ui_workflow_model_preview_ok(
    bool recording,
    bool stop_armed,
    uint64_t now_ms,
    uint64_t *armed_until_ms)
{
    if (!recording || armed_until_ms == NULL) return false;
    if (stop_armed && now_ms < *armed_until_ms) {
        *armed_until_ms = 0U;
        return true;
    }
    *armed_until_ms = now_ms + 2000U;
    return false;
}
