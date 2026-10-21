#include "ui_workflow_model.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

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
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_GPS_CHOICE,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_GPS_PENDING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_LAYOUT_READY,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RANGING_RESULT,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_SCAN_CHOICE,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_SCANNING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORD_READY,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RECORDING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_STOPPED,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_CALCULATING,
        UI_I18N_KEY_TFT_WORKFLOW_TITLE_RESULT,
    };
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    assert(WORKFLOW_STEP_COUNT == 19);
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
        WORKFLOW_COMMAND_PLACEMENT_CONFIRMED,
        WORKFLOW_COMMAND_GPS_SKIP,
        WORKFLOW_COMMAND_CALCULATE_POSITIONS,
        WORKFLOW_COMMAND_CONFIRM,
        WORKFLOW_COMMAND_START_RECORDING,
        WORKFLOW_COMMAND_STOP_RECORDING,
        WORKFLOW_COMMAND_MOVE_SENSORS,
        WORKFLOW_COMMAND_CALCULATE_SOURCES,
        WORKFLOW_COMMAND_DELETE_RECORDING,
        WORKFLOW_COMMAND_RECORD_AGAIN,
        WORKFLOW_COMMAND_SCAN_SKIP,
        WORKFLOW_COMMAND_BACK,
    };
    uint32_t allowed = 0U;
    for (size_t i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i) allowed |= WORKFLOW_ACTION(expected[i]);
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

    workflow_snapshot_t object_snapshot = make_snapshot(WORKFLOW_STEP_OBJECT);
    object_snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_START_MEASUREMENT);
    ui_workflow_model_state_t object_state = {0};
    build(&object_snapshot, &object_state, &ranging, &model);
    assert(object_snapshot.step == WORKFLOW_STEP_OBJECT);
    assert(model.action_count == 1U);
    assert(model.actions[0] == WORKFLOW_COMMAND_START_MEASUREMENT);
    ui_workflow_event_t start_event = ui_workflow_model_handle_key(
        &object_snapshot, &object_state, UI_WORKFLOW_KEY_OK, 99U);
    assert(start_event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
    assert(start_event.command == WORKFLOW_COMMAND_START_MEASUREMENT);
    assert(object_snapshot.step == WORKFLOW_STEP_OBJECT);

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

static void test_max460_guided_step_content_and_placement_gate(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_PLACEMENT);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    assert(model.line_count == 1U);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_WAITING);
    assert((model.actions_mask & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED)) == 0U);

    snapshot.placement.all_stable = true;
    snapshot.allowed_actions |= WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ALL_STABLE);
    assert((model.actions_mask & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED)) != 0U);

    snapshot = make_snapshot(WORKFLOW_STEP_GPS_CHOICE);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_GPS_WAITING);
    snapshot.step = WORKFLOW_STEP_GPS_PENDING;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_GPS_WAITING);

    snapshot = make_snapshot(WORKFLOW_STEP_LAYOUT_READY);
    snapshot.placement.all_stable = true;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_READY);
    snapshot.placement.all_stable = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_WAITING);

    snapshot = make_snapshot(WORKFLOW_STEP_SCAN_CHOICE);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_SCAN_PHONE);
    snapshot.step = WORKFLOW_STEP_SCANNING;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_SCAN_PHONE);
}

static void test_max460_tft_action_set_matches_core_for_guided_steps(void)
{
    static const workflow_step_t steps[] = {
        WORKFLOW_STEP_PLACEMENT,
        WORKFLOW_STEP_GPS_CHOICE,
        WORKFLOW_STEP_GPS_PENDING,
        WORKFLOW_STEP_LAYOUT_READY,
        WORKFLOW_STEP_SCAN_CHOICE,
        WORKFLOW_STEP_SCANNING,
    };
    ui_workflow_ranging_t ranging = {0};

    for (size_t step_index = 0U; step_index < sizeof(steps) / sizeof(steps[0]); ++step_index) {
        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = steps[step_index];
        core.calibration_ok = true;
        core.ranging_ok = true;

        workflow_snapshot_t snapshot;
        workflow_core_snapshot(&core, 4U, true, &snapshot);
        snapshot.placement.all_stable = true;

        ui_workflow_model_state_t state = {0};
        ui_workflow_model_t model;
        build(&snapshot, &state, &ranging, &model);
        assert(model.actions_mask == snapshot.allowed_actions);

        size_t expected_count = 0U;
        for (workflow_command_t command = 0; command < WORKFLOW_COMMAND_COUNT; ++command) {
            const bool allowed = (snapshot.allowed_actions & WORKFLOW_ACTION(command)) != 0U;
            bool offered = false;
            for (size_t action = 0U; action < model.action_count; ++action) {
                if (model.actions[action] == command) offered = true;
            }
            assert(offered == allowed);
            if (allowed) ++expected_count;
        }
        assert(model.action_count == expected_count);
    }
}

static void test_sensor_counts(void)
{
    static const ui_i18n_key_t expected[] = {
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_PLANAR,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_READY,
        UI_I18N_KEY_TFT_WORKFLOW_SENSOR_READY,
    };
    static const unsigned counts[] = {1U, 2U, 3U, 4U, 5U};
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

static void test_remote_slot_layout_is_fixed(void)
{
    static const uint8_t connected_masks[] = {
        0x00U,
        0x01U,
        0x07U,
        0x0FU,
        0x05U,
    };
    ui_workflow_remote_slot_t slots[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    for (size_t sample = 0U; sample < sizeof(connected_masks) / sizeof(connected_masks[0]); ++sample) {
        ui_workflow_remote_slot_layout(connected_masks[sample], slots);
        for (size_t slot = 0U; slot < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++slot) {
            assert(slots[slot].visible);
            assert(slots[slot].connected ==
                ((connected_masks[sample] & (uint8_t)(1U << slot)) != 0U));
        }
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
        if (step == WORKFLOW_STEP_OBJECT) {
            assert(event.type == UI_WORKFLOW_EVENT_LEAVE_HOME);
        } else {
            assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
            assert(event.command == WORKFLOW_COMMAND_BACK);
        }

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
        &snapshot, &state, UI_WORKFLOW_KEY_DOWN, 99U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    event = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED);
    assert(state.delete_confirm);

    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 101U);
    assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
    assert(event.command == WORKFLOW_COMMAND_DELETE_RECORDING);
    assert(!state.delete_confirm);

    state = (ui_workflow_model_state_t){0};
    (void)ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_DOWN, 99U);
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

/* MAX-491 fail-before(real): a pre-wrap async completion cannot clear seq 1. */
static void test_async_sequence_wrap_keeps_new_request_pending(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_OBJECT);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    state.has_pending = true;
    state.pending_seq = 1U;
    snapshot.last_async.seq = UINT32_MAX;
    snapshot.last_async.result = WORKFLOW_COMMAND_RESULT_OK;
    build(&snapshot, &state, &ranging, &model);
    assert(state.has_pending);

    snapshot.last_async.seq = 1U;
    build(&snapshot, &state, &ranging, &model);
    assert(!state.has_pending);
}

/* MAX-491 fail-before(real): a pre-wrap material result cannot resolve seq 1. */
static void test_material_save_sequence_wrap_keeps_new_save_pending(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_STOPPED);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    state.material_save_pending = true;
    state.material_save_seq = 1U;
    state.material_selection_index = 2U;
    state.material_view = UI_WORKFLOW_MATERIAL_SAVING;
    snapshot.last_material_save.seq = UINT32_MAX;
    snapshot.last_material_save.result = ESP_OK;
    build(&snapshot, &state, &ranging, &model);
    assert(state.material_save_pending);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_SAVING);
    assert(state.material_stored_index == 0U);

    snapshot.last_material_save.seq = 1U;
    build(&snapshot, &state, &ranging, &model);
    assert(!state.material_save_pending);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CLOSED);
    assert(state.material_stored_index == 2U);
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


static void test_sensor_check_action_and_warning_for_low_remote_counts(void)
{
    for (uint8_t remotes = 0U; remotes < 3U; ++remotes) {
        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = WORKFLOW_STEP_SENSOR_CHECK;

        workflow_snapshot_t snapshot;
        workflow_core_snapshot(&core, remotes, false, &snapshot);
        ui_workflow_model_state_t state = {0};
        ui_workflow_ranging_t ranging = {0};
        ui_workflow_model_t model;
        build(&snapshot, &state, &ranging, &model);

        const bool has_calibration_action =
            (model.actions_mask & WORKFLOW_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION)) != 0U;
#if CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
        assert(!has_calibration_action);
#else
        assert(has_calibration_action);
#endif
        bool has_insufficient_warning = false;
        for (size_t i = 0U; i < model.line_count; ++i) {
            has_insufficient_warning = has_insufficient_warning ||
                model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT;
        }
        assert(has_insufficient_warning);
    }
}


static void test_workflow_start_and_preview_regressions(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    workflow_command_params_t params = {0};
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_START_MEASUREMENT,
        &params, 2U, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_SENSOR_CHECK);

    const workflow_step_t preview_steps[] = {
        WORKFLOW_STEP_RECORD_READY,
        WORKFLOW_STEP_RECORDING,
        WORKFLOW_STEP_RESULT,
    };
    for (size_t i = 0U; i < sizeof(preview_steps) / sizeof(preview_steps[0]); ++i) {
        workflow_snapshot_t snapshot = make_snapshot(preview_steps[i]);
        snapshot.has_scan = true;
        ui_workflow_model_state_t state = {0};
        ui_workflow_event_t event = ui_workflow_model_handle_key(
            &snapshot, &state, UI_WORKFLOW_KEY_RIGHT, 100U);
        assert(event.type == UI_WORKFLOW_EVENT_OPEN_PREVIEW);
    }
}


static void test_insufficient_nodes_results_use_existing_warning(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    snapshot.calibration.result_valid = true;
    snapshot.calibration.result_ok = false;
    snapshot.calibration.reasons_count = 1U;
    (void)snprintf(snapshot.calibration.reasons[0], WORKFLOW_CALIBRATION_REASON_MAX,
        "insufficient_nodes");
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    build(&snapshot, &state, &ranging, &model);
    assert(model.line_count == 2U);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT);

    snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    (void)snprintf(snapshot.reason, sizeof(snapshot.reason), "insufficient_nodes");
    build(&snapshot, &state, &ranging, &model);
    assert(model.line_count == 2U);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_SENSOR_INSUFFICIENT);
}

int main(void)
{
#ifdef MAX449_CASE
#if MAX449_CASE == 6
    test_sensor_check_action_and_warning_for_low_remote_counts();
    test_remote_slot_layout_is_fixed();
#elif MAX449_CASE == 7
    test_workflow_start_and_preview_regressions();
#elif MAX449_CASE == 8
    test_insufficient_nodes_results_use_existing_warning();
#else
#error "unknown MAX449_CASE for ui_workflow_model_test"
#endif
#else
    test_all_steps();
    test_actions_follow_allowed_mask();
    test_async_sequence_wrap_keeps_new_request_pending();
    test_material_save_sequence_wrap_keeps_new_save_pending();
    test_max460_guided_step_content_and_placement_gate();
    test_max460_tft_action_set_matches_core_for_guided_steps();
    test_sensor_counts();
    test_remote_slot_layout_is_fixed();
    test_left_behavior_each_step();
    test_right_preview_matrix();
    test_delete_confirmation();
    test_pending_notices();
    test_unknown_values_and_ranging_sigma();
    test_calibration_phase_and_result_status();
    test_calibration_reason_mapping();
    test_result_routing_and_distances();
    test_preview_stop_double_ok();
    test_sensor_check_action_and_warning_for_low_remote_counts();
    test_workflow_start_and_preview_regressions();
    test_insufficient_nodes_results_use_existing_warning();
    return 0;
#endif
}
