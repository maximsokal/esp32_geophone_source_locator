#include "ui_workflow_model.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static workflow_snapshot_t make_snapshot(workflow_step_t step)
{
    workflow_snapshot_t snapshot = {0};
    snapshot.step = step;
    snapshot.progress = WORKFLOW_PROGRESS_UNKNOWN;
    return snapshot;
}

static void build(
    workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_ranging_t *ranging,
    ui_workflow_model_t *model)
{
    ui_workflow_model_build(snapshot, ranging, state, 100U, model);
}

static void test_all_steps(void)
{
    static const ui_i18n_key_t expected[WORKFLOW_STEP_COUNT] = {
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_IDLE,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_OBJECT,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_SENSOR_CHECK,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIB_PREP,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIBRATING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALIB_RESULT,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_PLACEMENT,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING_RESULT,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORD_READY,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORDING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_STOPPED,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALCULATING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RESULT,
    };
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    assert(WORKFLOW_STEP_COUNT == 14);
    for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
        workflow_snapshot_t snapshot = make_snapshot(step);
        build(&snapshot, &state, &ranging, &model);
        assert(model.title_key == expected[step]);
        assert(model.line_count > 0U || model.progress_indeterminate);
    }
}

static void test_actions_follow_allowed_mask(void)
{
    static const workflow_command_t expected[] = {
        WORKFLOW_COMMAND_START_MEASUREMENT,
        WORKFLOW_COMMAND_GO_CALIBRATION,
        WORKFLOW_COMMAND_SENSORS_PLACED,
        WORKFLOW_COMMAND_RETRY,
        WORKFLOW_COMMAND_PLACE_SENSORS,
        WORKFLOW_COMMAND_CALCULATE_POSITIONS,
        WORKFLOW_COMMAND_CONFIRM,
        WORKFLOW_COMMAND_START_RECORDING,
        WORKFLOW_COMMAND_STOP_RECORDING,
        WORKFLOW_COMMAND_MOVE_SENSORS,
        WORKFLOW_COMMAND_CALCULATE_SOURCES,
        WORKFLOW_COMMAND_DELETE_RECORDING,
        WORKFLOW_COMMAND_RECORD_AGAIN,
        WORKFLOW_COMMAND_BACK,
    };
    uint32_t allowed = 0U;
    for (workflow_command_t command = WORKFLOW_COMMAND_START_MEASUREMENT;
         command < WORKFLOW_COMMAND_COUNT; ++command) {
        if (command != WORKFLOW_COMMAND_CALIBRATION_DONE &&
            command != WORKFLOW_COMMAND_RANGING_DONE &&
            command != WORKFLOW_COMMAND_CALCULATION_DONE) {
            allowed |= WORKFLOW_ACTION(command);
        }
    }
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_OBJECT);
    snapshot.allowed_actions = allowed;
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    build(&snapshot, &state, &ranging, &model);
    assert(model.actions_mask == allowed);
    assert(model.action_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0U; i < model.action_count; ++i) assert(model.actions[i] == expected[i]);
    assert(model.selected_action == 0U);

    ui_workflow_event_t event = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_DOWN, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    build(&snapshot, &state, &ranging, &model);
    assert(model.selected_action == 1U);
    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_UP, 101U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    build(&snapshot, &state, &ranging, &model);
    assert(model.selected_action == 0U);
    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_UP, 102U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    build(&snapshot, &state, &ranging, &model);
    assert(model.selected_action == model.action_count - 1U);

    snapshot.allowed_actions = 0U;
    build(&snapshot, &state, &ranging, &model);
    assert(model.action_count == 0U);
    assert(model.actions_mask == 0U);
}

static void test_sensor_counts(void)
{
    static const ui_i18n_key_t expected[] = {
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_PLANAR,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_READY,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_READY,
    };
    static const unsigned counts[] = {2U, 3U, 4U, 5U};
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    for (size_t i = 0U; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_SENSOR_CHECK);
        snapshot.assigned_remotes = (uint8_t)counts[i];
        build(&snapshot, &state, &ranging, &model);
        assert(model.line_count == 2U);
        assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_SENSOR_REMOTE_COUNT);
        char actual[12];
        (void)snprintf(actual, sizeof(actual), "%u", counts[i]);
        assert(strcmp(model.lines[0].args[0], actual) == 0);
        assert(model.lines[1].key == expected[i]);
    }
}

static void test_left_behavior_each_step(void)
{
    ui_workflow_model_state_t state = {0};
    for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
        workflow_snapshot_t snapshot = make_snapshot(step);
        ui_workflow_event_t event;
        snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
        event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_LEFT, 100U);
        assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
        assert(event.command == WORKFLOW_COMMAND_BACK);

        snapshot.allowed_actions = 0U;
        event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_LEFT, 100U);
        if (step == WORKFLOW_STEP_IDLE || step == WORKFLOW_STEP_OBJECT || step == WORKFLOW_STEP_SENSOR_CHECK) {
            assert(event.type == UI_WORKFLOW_EVENT_LEAVE_OBJECTS);
        } else {
            assert(event.type == UI_WORKFLOW_EVENT_NONE);
        }
    }
}

static void test_right_preview_matrix(void)
{
    ui_workflow_model_state_t state = {0};
    for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
        for (unsigned has_scan = 0U; has_scan < 2U; ++has_scan) {
            workflow_snapshot_t snapshot = make_snapshot(step);
            snapshot.has_scan = has_scan != 0U;
            const ui_workflow_event_t event = ui_workflow_model_handle_key(
                &snapshot, &state, UI_WORKFLOW_KEY_RIGHT, 100U);
            const bool available = snapshot.has_scan && (step == WORKFLOW_STEP_RECORD_READY ||
                step == WORKFLOW_STEP_RECORDING || step == WORKFLOW_STEP_RESULT);
            assert((event.type == UI_WORKFLOW_EVENT_OPEN_PREVIEW) == available);
        }
    }
}

static void test_delete_confirmation(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_STOPPED);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_DELETE_RECORDING);
    ui_workflow_model_state_t state = {0};
    ui_workflow_event_t event = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED);
    assert(state.delete_confirm);

    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 101U);
    assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
    assert(event.command == WORKFLOW_COMMAND_DELETE_RECORDING);
    assert(!state.delete_confirm);

    state = (ui_workflow_model_state_t){0};
    (void)ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 100U);
    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_LEFT, 101U);
    assert(event.type == UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED);
    assert(!state.delete_confirm);
}

static void test_pending_notices(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RECORDING);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_STOP_RECORDING);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    ui_workflow_model_command_sent(&state, WORKFLOW_COMMAND_STOP_RECORDING, ESP_OK, 7U, 100U);
    snapshot.last_async.seq = 6U;
    snapshot.last_async.result = WORKFLOW_COMMAND_RESULT_OK;
    build(&snapshot, &state, &ranging, &model);
    assert(model.pending);
    assert(state.has_pending);
    assert(ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 101U).type ==
        UI_WORKFLOW_EVENT_NONE);

    snapshot.last_async.seq = 7U;
    snapshot.last_async.result = WORKFLOW_COMMAND_RESULT_STALE;
    build(&snapshot, &state, &ranging, &model);
    assert(!model.pending);
    assert(!state.has_pending);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED);
    build(&snapshot, &state, &ranging, &model);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED);
    build(&snapshot, &state, &ranging, &model);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED);

    ui_workflow_model_command_sent(&state, WORKFLOW_COMMAND_STOP_RECORDING, ESP_OK, 8U, 150U);
    snapshot.last_async.seq = 8U;
    snapshot.last_async.result = WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    build(&snapshot, &state, &ranging, &model);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_STATE_CHANGED);

    ui_workflow_model_command_sent(&state, WORKFLOW_COMMAND_STOP_RECORDING, ESP_ERR_NO_MEM, 0U, 200U);
    assert(state.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_BUSY);
    state.notice_until_ms = 1000U;
    build(&snapshot, &state, &ranging, &model);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_BUSY);

    snapshot.last_async.result = WORKFLOW_COMMAND_RESULT_OK;
    ui_workflow_model_command_sent(&state, WORKFLOW_COMMAND_STOP_RECORDING, ESP_OK, 9U, 100U);
    snapshot.last_async.seq = 9U;
    build(&snapshot, &state, &ranging, &model);
    assert(!model.pending);
    assert(model.notice_key == UI_I18N_KEY_COUNT);
}

static void test_unknown_values_and_ranging_sigma(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;

    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_OBJECT);
    (void)snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ-123");
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(strcmp(model.lines[0].args[0], "0") != 0);
    assert(strcmp(model.lines[0].args[0], "OBJ-123") != 0);

    snapshot = make_snapshot(WORKFLOW_STEP_CALIBRATING);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(model.lines[0].arg_keys[3] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);

    snapshot = make_snapshot(WORKFLOW_STEP_PLACEMENT);
    snapshot.placement.per_node_count = 1U;
    (void)snprintf(snapshot.placement.per_node[0].node, sizeof(snapshot.placement.per_node[0].node), "G1");
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[2] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(model.lines[0].arg_keys[3] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);

    snapshot = make_snapshot(WORKFLOW_STEP_RANGING);
    ranging.available = true;
    ranging.pairs_done = 2U;
    ranging.pairs_total = 5U;
    ranging.node_count = 1U;
    (void)snprintf(ranging.nodes[0], sizeof(ranging.nodes[0]), "G1");
    ranging.sigma_mm[0] = INT32_MAX;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_SIGMA);
    assert(model.lines[1].arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(strcmp(model.lines[1].args[1], "0") != 0);

    snapshot = make_snapshot(WORKFLOW_STEP_RECORDING);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
}

static void test_calibration_phase_and_result_status(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIBRATING);
    (void)snprintf(snapshot.calibration.phase, sizeof(snapshot.calibration.phase), "running");
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_PHASE_RUNNING);

    (void)snprintf(snapshot.calibration.phase, sizeof(snapshot.calibration.phase), "unknown-phase");
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);

    snapshot = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    snapshot.calibration.result_valid = true;
    snapshot.calibration.result_ok = true;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_OK);
    snapshot.calibration.result_ok = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_FAIL);
    snapshot.calibration.result_valid = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
}

static void test_calibration_reason_mapping(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    snapshot.calibration.reasons_count = 3U;
    (void)snprintf(snapshot.calibration.reasons[0], WORKFLOW_CALIBRATION_REASON_MAX,
        "G2:height_rejected_moving");
    (void)snprintf(snapshot.calibration.reasons[1], WORKFLOW_CALIBRATION_REASON_MAX,
        "cs_spread_too_high");
    (void)snprintf(snapshot.calibration.reasons[2], WORKFLOW_CALIBRATION_REASON_MAX,
        "vendor_reason:raw detail");
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_REASON);
    assert(strcmp(model.lines[1].args[0], "G2") == 0);
    assert(model.lines[1].arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_REASON_HEIGHT_MOVING);
    assert(model.lines[2].key == UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_REASON_SINGLE);
    assert(model.lines[2].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_REASON_CS_SPREAD);
    assert(model.lines[3].key == UI_I18N_KEY_TFT_WORKFLOW_RESULT_REASON);
    assert(strcmp(model.lines[3].args[0], "vendor_reason:raw detail") == 0);
}

static void test_result_routing_and_distances(void)
{
    static const workflow_calculation_class_t classes[] = {
        WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN,
        WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS,
        WORKFLOW_CALCULATION_CLASS_INTERNAL,
    };
    static const ui_i18n_key_t result_keys[] = {
        UI_I18N_KEY_TFT_WORKFLOW_RESULT_RECORD_AGAIN,
        UI_I18N_KEY_TFT_WORKFLOW_RESULT_MOVE_SENSORS,
        UI_I18N_KEY_TFT_WORKFLOW_RESULT_INTERNAL,
    };
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RESULT);
    for (size_t i = 0U; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        snapshot.calculation_class = classes[i];
        build(&snapshot, &state, &ranging, &model);
        assert(model.lines[0].key == result_keys[i]);
    }

    snapshot.calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    state.result_data_ready = true;
    state.result_waveform_available = true;
    state.result_source_valid = true;
    state.result_source_m[0] = 0.0;
    state.result_source_m[1] = 0.0;
    state.result_source_m[2] = 0.0;
    state.result_geometry.pods[0].computed = true;
    state.result_geometry.pods[0].x_mm = 3000;
    state.result_geometry.pods[0].y_mm = 4000;
    state.result_geometry.pods[1].computed = true;
    state.result_geometry.pods[1].z_mm = 2000;
    build(&snapshot, &state, &ranging, &model);

    result_view_distance_t distances[OBJECT_STORE_POD_COUNT] = {0};
    assert(result_view_source_distances(state.result_source_m, state.result_geometry.pods,
        OBJECT_STORE_POD_COUNT, distances) == RESULT_VIEW_OK);
    const ui_workflow_model_line_t *first = &model.lines[1];
    const ui_workflow_model_line_t *second = &model.lines[2];
    const ui_workflow_model_line_t *third = &model.lines[3];
    assert(first->key == UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE);
    assert(second->key == UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE);
    assert(third->key == UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN);
    char expected_distance[20];
    (void)snprintf(expected_distance, sizeof(expected_distance), "%.1f", distances[0].distance_m);
    assert(strcmp(first->args[1], expected_distance) == 0);
    (void)snprintf(expected_distance, sizeof(expected_distance), "%.1f", distances[1].distance_m);
    assert(strcmp(second->args[1], expected_distance) == 0);
    assert(first->arg_keys[1] == UI_I18N_KEY_COUNT);
    assert(third->arg_keys[1] == UI_I18N_KEY_COUNT);

    state.result_source_valid = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN);
}

static void test_preview_stop_double_ok(void)
{
    uint64_t deadline = 0U;
    assert(!ui_workflow_model_preview_ok(true, false, 100U, &deadline));
    assert(deadline == 2100U);
    assert(ui_workflow_model_preview_ok(true, true, 200U, &deadline));
    assert(deadline == 0U);
    assert(!ui_workflow_model_preview_ok(true, true, 2200U, &deadline));
    assert(deadline == 4200U);
    assert(!ui_workflow_model_preview_ok(false, false, 5000U, &deadline));
}

int main(void)
{
    test_all_steps();
    test_actions_follow_allowed_mask();
    test_sensor_counts();
    test_left_behavior_each_step();
    test_right_preview_matrix();
    test_delete_confirmation();
    test_pending_notices();
    test_unknown_values_and_ranging_sigma();
    test_calibration_phase_and_result_status();
    test_calibration_reason_mapping();
    test_result_routing_and_distances();
    test_preview_stop_double_ok();
    return 0;
}
