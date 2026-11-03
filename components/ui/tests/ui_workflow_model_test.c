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
        WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI,
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
    assert(model.line_count == 3U);                         /* the Wi-Fi choice below is explained on the page */
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_WIFI_HINT);
    assert(model.lines[2].key == UI_I18N_KEY_TFT_WORKFLOW_LAYOUT_WIFI_PHONE);
    snapshot.placement.all_stable = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_WAITING);
    assert(model.line_count == 1U);                         /* nothing to choose yet */

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
        /* The only entry that is not a firmware command here is the live-signals view on the ranging result. */
        if (steps[step_index] == WORKFLOW_STEP_RANGING_RESULT) ++expected_count;
        assert(model.action_count == expected_count);
    }
}

/*
 * Starting the distance measurement is an explicit choice made on the page: the first action turns Wi-Fi off, the second keeps it on,
 * and OK sends exactly the highlighted one.
 */
static void test_the_wifi_choice_before_the_distance_measurement_is_explicit(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_LAYOUT_READY;
    workflow_snapshot_t snapshot;
    workflow_core_snapshot(&core, 4U, true, &snapshot);
    snapshot.placement.all_stable = true;

    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;
    build(&snapshot, &state, &ranging, &model);
    assert(model.action_count == 3U);
    assert(model.actions[0] == WORKFLOW_COMMAND_CALCULATE_POSITIONS);
    assert(model.actions[1] == WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI);
    assert(model.actions[2] == WORKFLOW_COMMAND_BACK);
    assert(model.selected_action == 0U);

    ui_workflow_event_t off = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 10U);
    assert(off.type == UI_WORKFLOW_EVENT_SEND_COMMAND && off.command == WORKFLOW_COMMAND_CALCULATE_POSITIONS);

    ui_workflow_model_state_t second = {0};
    build(&snapshot, &second, &ranging, &model);
    (void)ui_workflow_model_handle_key(&snapshot, &second, UI_WORKFLOW_KEY_DOWN, 20U);
    ui_workflow_event_t keep = ui_workflow_model_handle_key(&snapshot, &second, UI_WORKFLOW_KEY_OK, 30U);
    assert(keep.type == UI_WORKFLOW_EVENT_SEND_COMMAND && keep.command == WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI);
}

static void set_pair(ui_workflow_ranging_t *ranging, size_t index, unsigned a, unsigned b, bool valid, bool cable, bool manual,
    int32_t distance_mm, unsigned cause, unsigned ab_samples, unsigned ba_samples, int32_t ab_mm, int32_t ba_mm)
{
    memset(&ranging->pairs[index], 0, sizeof(ranging->pairs[index]));
    ranging->pairs[index].node_a = (uint8_t)a;
    ranging->pairs[index].node_b = (uint8_t)b;
    ranging->pairs[index].valid = valid;
    ranging->pairs[index].cable_exceeded = cable;
    ranging->pairs[index].manual = manual;
    ranging->pairs[index].distance_mm = distance_mm;
    ranging->pairs[index].cause = (uint8_t)cause;
    ranging->pairs[index].diag_ab_samples = (uint8_t)ab_samples;
    ranging->pairs[index].diag_ba_samples = (uint8_t)ba_samples;
    ranging->pairs[index].diag_ab_mm = ab_mm;
    ranging->pairs[index].diag_ba_mm = ba_mm;
}

/*
 * Bench 2026-11-03: MAIN-G4 had no distance and the screen showed "0-4: --" although MAIN->G4 had produced 8.8 m and G4->MAIN 6.4 m.
 * The pair row now shows what each direction measured and a second row says why it was not used; valid pairs, a cable violation and a
 * pair without any diagnostics keep the rows they had.
 */
static void test_a_ranging_pair_without_a_distance_shows_both_directions_and_the_cause(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;

    ranging.available = true;
    ranging.pairs_done = ranging.pairs_total = 3U;
    ranging.pair_count = 4U;
    set_pair(&ranging, 0U, 0U, 1U, true, false, false, 4977, 0U, 0U, 0U, 0, 0);
    set_pair(&ranging, 1U, 0U, 4U, false, false, false, 0, 2U, 3U, 10U, 8845, 6421);
    set_pair(&ranging, 2U, 1U, 3U, false, false, false, 0, 1U, 0U, 9U, 0, 4630);
    set_pair(&ranging, 3U, 3U, 4U, false, false, false, 0, 0U, 0U, 0U, 0, 0);                   /* an old device: nothing reported */
    build(&snapshot, &state, &ranging, &model);

    size_t diag_rows = 0U, cause_rows = 0U, plain_rows = 0U;
    const ui_workflow_model_line_t *diag = NULL, *half = NULL;
    for (size_t i = 0U; i < model.line_count; ++i) {
        if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIR_DIAG) {
            if (diag == NULL) diag = &model.lines[i]; else half = &model.lines[i];
            ++diag_rows;
        } else if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_CAUSE_LOW_CONFIDENCE ||
                   model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_CAUSE_NO_MEASUREMENT) {
            ++cause_rows;
        } else if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_DISTANCE) {
            ++plain_rows;
        }
    }
    assert(diag_rows == 2U && cause_rows == 2U);
    assert(plain_rows == 2U);                       /* the valid pair and the pair that reported nothing keep the distance row */
    assert(strcmp(diag->args[0], "0") == 0 && strcmp(diag->args[1], "4") == 0);
    assert(strcmp(diag->args[2], "885") == 0 && strcmp(diag->args[3], "642") == 0);           /* cm, rounded */
    assert(half != NULL && half->arg_keys[2] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);       /* the direction with no result */
    assert(strcmp(half->args[3], "463") == 0);
    for (size_t i = 0U; i + 1U < model.line_count; ++i) {                                      /* the cause row follows its pair row */
        if (&model.lines[i] == diag) assert(model.lines[i + 1U].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_CAUSE_LOW_CONFIDENCE);
        if (&model.lines[i] == half) assert(model.lines[i + 1U].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_CAUSE_NO_MEASUREMENT);
    }

    /* A cable violation keeps its row, even with diagnostics; so does a manual distance. */
    ranging.pair_count = 2U;
    set_pair(&ranging, 0U, 0U, 4U, false, true, false, 6000, 5U, 3U, 0U, 6000, 0);
    set_pair(&ranging, 1U, 1U, 2U, true, false, true, 1500, 0U, 3U, 0U, 9000, 0);
    build(&snapshot, &state, &ranging, &model);
    for (size_t i = 0U; i < model.line_count; ++i) assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIR_DIAG);
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
        if (step == WORKFLOW_STEP_OBJECT) {
            assert(event.type == UI_WORKFLOW_EVENT_LEAVE_HOME);
        } else if (step == WORKFLOW_STEP_IDLE || step == WORKFLOW_STEP_SENSOR_CHECK) {
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

    snapshot = make_snapshot(WORKFLOW_STEP_PLACEMENT);
    snapshot.placement.per_node_count = 1U;
    (void)snprintf(snapshot.placement.per_node[0].node, sizeof(snapshot.placement.per_node[0].node), "G1");
    build(&snapshot, &state, &ranging, &model);
    /* The node is not stable, so its blocker line comes first and the row follows. */
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_BLOCKED);
    assert(model.lines[1].arg_keys[2] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(model.lines[1].arg_keys[3] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);

    snapshot = make_snapshot(WORKFLOW_STEP_RANGING);
    ranging.available = true;
    ranging.pairs_done = 2U;
    ranging.pairs_total = 5U;
    ranging.node_count = 1U;
    (void)snprintf(ranging.nodes[0], sizeof(ranging.nodes[0]), "G1");
    ranging.sigma_mm[0] = INT32_MAX;
    build(&snapshot, &state, &ranging, &model);
    /* The nodes range over radio on their own: the screen says so right under the pair counter. */
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIRS);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT_PERCENT);
    assert(strcmp(model.lines[1].args[0], "40") == 0); /* 2 of 5 windows */
    assert(model.lines[2].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_SIGMA);
    assert(model.lines[2].arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
    assert(strcmp(model.lines[2].args[1], "0") != 0);
    assert(model.line_count == 3U);

    /* The plan holds one execution per ordered pair of the connected nodes: 3 nodes 6, 4 nodes 12,
     * 5 nodes 20. The same three finished windows are a smaller share of a bigger network. */
    {
        static const struct { size_t total; const char *percent; } by_nodes[] = {
            { 6U, "50" }, { 12U, "25" }, { 20U, "15" },
        };
        for (size_t nodes = 3U; nodes <= 5U; ++nodes) {
            ranging.node_count = 0U;
            ranging.pairs_total = nodes * (nodes - 1U);
            ranging.pairs_done = 3U;
            assert(ranging.pairs_total == by_nodes[nodes - 3U].total);
            build(&snapshot, &state, &ranging, &model);
            assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT_PERCENT);
            assert(strcmp(model.lines[1].args[0], by_nodes[nodes - 3U].percent) == 0);
        }
    }

    /* Percent follows the reported windows: none, half, all, and never above 100 or from nothing. */
    snapshot = make_snapshot(WORKFLOW_STEP_RANGING);
    ranging.node_count = 0U;
    ranging.pairs_total = 30U;
    ranging.pairs_done = 0U;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT_PERCENT);
    assert(strcmp(model.lines[1].args[0], "0") == 0);
    ranging.pairs_done = 15U;
    build(&snapshot, &state, &ranging, &model);
    assert(strcmp(model.lines[1].args[0], "50") == 0);
    ranging.pairs_done = 30U;
    build(&snapshot, &state, &ranging, &model);
    assert(strcmp(model.lines[1].args[0], "100") == 0);
    ranging.pairs_done = 31U;
    build(&snapshot, &state, &ranging, &model);
    assert(strcmp(model.lines[1].args[0], "100") == 0);
    ranging.pairs_total = 0U;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT);
    ranging.available = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT);

    snapshot = make_snapshot(WORKFLOW_STEP_RECORDING);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN);
}

static void test_calibration_result_points_to_placing_the_geophones_next(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    snapshot.calibration.result_valid = true;
    snapshot.calibration.result_ok = true;
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACE_SENSORS) | WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    /* The operator is told what comes after the calibration: placing the geophones, not the live view. */
    assert(model.line_count >= 2U);
    assert(model.lines[model.line_count - 1U].key == UI_I18N_KEY_TFT_WORKFLOW_CALIB_RESULT_NEXT);

    /* A failed calibration offers a retry, not the next step: no such line. */
    snapshot.calibration.result_ok = false;
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY) | WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    for (size_t i = 0U; i < model.line_count; ++i) {
        assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_CALIB_RESULT_NEXT);
    }
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

/* MAIN has no IMU: its calibration row must be the IMU-free variant, remotes keep I:/b:. */
static void test_calibration_main_row_has_no_imu_fields(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIBRATING);
    snapshot.calibration.per_node_count = 2U;
    workflow_calibration_node_t *main_node = &snapshot.calibration.per_node[0];
    (void)snprintf(main_node->node, sizeof(main_node->node), "MAIN");
    (void)snprintf(main_node->height_state, sizeof(main_node->height_state), "valid");
    (void)snprintf(main_node->imu_state, sizeof(main_node->imu_state), "not_applicable");
    main_node->relative_z_mm_valid = true;
    main_node->relative_z_mm = 120;
    workflow_calibration_node_t *remote = &snapshot.calibration.per_node[1];
    (void)snprintf(remote->node, sizeof(remote->node), "G1");
    (void)snprintf(remote->height_state, sizeof(remote->height_state), "valid");
    (void)snprintf(remote->imu_state, sizeof(remote->imu_state), "calibrated");
    remote->imu_values_valid = true;
    remote->relative_z_mm_valid = true;
    remote->relative_z_mm = 5;

    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    build(&snapshot, &state, &ranging, &model);
    assert(model.line_count == 3U);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_ROW_MAIN);
    assert(strcmp(model.lines[1].args[0], "MAIN") == 0);
    assert(model.lines[1].arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_STATE_VALID);
    assert(strcmp(model.lines[1].args[2], "12") == 0);    /* 120 mm, shown in centimetres */
    assert(strcmp(model.lines[2].args[2], "1") == 0);     /* 5 mm rounds to the nearest centimetre */
    assert(model.lines[2].key == UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_ROW);
    assert(model.lines[2].arg_keys[3] == UI_I18N_KEY_TFT_WORKFLOW_STATE_CALIBRATED);
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
        assert(has_calibration_action);
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

/* The Wi-Fi radio is off while the nodes range: the page says so before it goes off, while it is off, and while it comes back. */
static void test_ranging_page_says_what_the_wifi_radio_is_doing(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING);
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;

    /* Pending: the notice comes first, also while no progress exists (the nodes wait for the radio). */
    ui_workflow_ranging_t ranging = {.radio_state = UI_WORKFLOW_RADIO_PENDING};
    build(&snapshot, &state, &ranging, &model);
    assert(model.line_count >= 1U && model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_PENDING);
    ranging.radio_state = UI_WORKFLOW_RADIO_OFF;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_OFF);
    /* The ordinary lines are still there after it. */
    bool saw_wait = false;
    for (size_t i = 1U; i < model.line_count; ++i) {
        saw_wait = saw_wait || model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WAIT;
    }
    assert(saw_wait);

    /* With progress the notice is still the first line. */
    ranging.available = true;
    ranging.pairs_total = 12U;
    ranging.pairs_done = 3U;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_OFF);

    /* Idle, or no radio information at all: no Wi-Fi line, as before. */
    ranging.radio_state = UI_WORKFLOW_RADIO_IDLE;
    build(&snapshot, &state, &ranging, &model);
    for (size_t i = 0U; i < model.line_count; ++i) {
        assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_OFF);
        assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_PENDING);
        assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_BACK);
    }
    ranging.radio_state = UI_WORKFLOW_RADIO_RESUMING;       /* resuming is not shown on the measurement page itself */
    build(&snapshot, &state, &ranging, &model);
    for (size_t i = 0U; i < model.line_count; ++i) assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_BACK);

    /* The result page: the radio comes back, and says so while it does. */
    snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    ranging = (ui_workflow_ranging_t){.radio_state = UI_WORKFLOW_RADIO_RESUMING, .available = true, .pairs_total = 12U,
        .pairs_done = 12U};
    build(&snapshot, &state, &ranging, &model);
    bool saw_back = false;
    for (size_t i = 0U; i < model.line_count; ++i) saw_back = saw_back || model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_BACK;
    assert(saw_back);
    ranging.radio_state = UI_WORKFLOW_RADIO_IDLE;
    build(&snapshot, &state, &ranging, &model);
    for (size_t i = 0U; i < model.line_count; ++i) assert(model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_WIFI_BACK);
}

static void test_ranging_manual_entry(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {.available = true, .pair_count = 2U, .started_at_ms = 10U, .updated_at_ms = 20U};
    ranging.pairs[0].node_a = 0U; ranging.pairs[0].node_b = 1U; ranging.pairs[0].valid = true;
    ranging.pairs[1].node_a = 0U; ranging.pairs[1].node_b = 2U;
    ranging.pairs[1].cable_exceeded = true; ranging.pairs[1].distance_mm = 6001;
    ui_workflow_distance_open(&snapshot, &ranging, &state);
    assert(state.distance_view == UI_WORKFLOW_DISTANCE_PAIR && state.distance_pair == 1U);
    ui_workflow_event_t event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_UP, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE && state.distance_pair == 0U);
    (void)ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_DOWN, 100U);
    (void)ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(state.distance_view == UI_WORKFLOW_DISTANCE_VALUE);
    (void)ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_RIGHT, 100U);
    (void)ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_RIGHT, 100U);
    for (unsigned i = 0U; i < 5U; ++i)
        (void)ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_DOWN, 100U);
    assert(state.distance_mm == 1001U);
    state.distance_mm = 0U;
    event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE && state.distance_view == UI_WORKFLOW_DISTANCE_VALUE);
    state.distance_mm = 1500U;
    event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_SAVE_DISTANCE);
    assert(event.distance.node_a == 0U && event.distance.node_b == 2U && event.distance.distance_mm == 1500);
    assert(event.distance.expected_generation == snapshot.generation);
    assert(event.distance.ranging_started_at_ms == 10U && event.distance.ranging_updated_at_ms == 20U);
    assert(state.distance_view == UI_WORKFLOW_DISTANCE_SAVING);
    event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    state.distance_view = UI_WORKFLOW_DISTANCE_CLOSED;
    ui_workflow_distance_open(&snapshot, &ranging, &state);
    event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_LEFT, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE && state.distance_view == UI_WORKFLOW_DISTANCE_CLOSED);
    ui_workflow_distance_open(&snapshot, &ranging, &state);
    ++snapshot.generation;
    event = ui_workflow_distance_handle_key(&snapshot, &ranging, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE && state.distance_view == UI_WORKFLOW_DISTANCE_CLOSED);
}

static void test_ranging_result_three_outcomes(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {.available = true};
    ui_workflow_model_t model;
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_SUCCESS);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY);
    ranging.partial = true;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_PARTIAL);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_PARTIAL);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY);
    ranging.partial = false;
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_FAILED);
}

static void test_ranging_result_displays_pair_values(void)
{
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {.available = true, .pair_count = 2U};
    ui_workflow_model_t model;
    ranging.pairs[0].node_a = 0U; ranging.pairs[0].node_b = 1U;
    ranging.pairs[0].valid = true; ranging.pairs[0].distance_mm = 1234;
    ranging.pairs[1].node_a = 0U; ranging.pairs[1].node_b = 2U;
    build(&snapshot, &state, &ranging, &model);
    size_t found = 0U;
    for (size_t i = 0U; i < model.line_count; ++i) {
        if (model.lines[i].key != UI_I18N_KEY_TFT_WORKFLOW_RANGING_DISTANCE) continue;
        assert(strcmp(model.lines[i].args[0], "0") == 0);
        assert(strcmp(model.lines[i].args[1], found == 0U ? "1" : "2") == 0);
        if (found == 0U) assert(strcmp(model.lines[i].args[2], "123") == 0);   /* 1234 mm, shown in centimetres */
        else assert(model.lines[i].arg_keys[2] != UI_I18N_KEY_COUNT);
        ++found;
    }
    assert(found == 2U);
}

static void test_selection_does_not_stick_to_back_after_a_step_with_only_back(void)
{
    /* 2026-10-31 16:24: after the calibration run the highlighted action on the result screen was BACK, so
     * OK walked the user back instead of to the placement step ("no button to continue"). While the run is
     * in progress BACK is the only action, the selection fell onto it and was carried to the next step. */
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;

    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_CALIB_PREP);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_SENSORS_PLACED) | WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    assert(model.actions[model.selected_action] == WORKFLOW_COMMAND_SENSORS_PLACED);

    snapshot = make_snapshot(WORKFLOW_STEP_CALIBRATING);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    assert(model.action_count == 1U && model.actions[0] == WORKFLOW_COMMAND_BACK);

    snapshot = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    snapshot.calibration.result_valid = true;
    snapshot.calibration.result_ok = true;
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK) | WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACE_SENSORS);
    build(&snapshot, &state, &ranging, &model);
    /* After the calibration the next step is placing the geophones; the live signals come after the positions. */
    assert(model.action_count == 2U);
    assert(model.actions[0] == WORKFLOW_COMMAND_PLACE_SENSORS);
    assert(model.actions[1] == WORKFLOW_COMMAND_BACK);
    assert(model.actions[model.selected_action] == WORKFLOW_COMMAND_PLACE_SENSORS);
    ui_workflow_event_t event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 200U);
    assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
    assert(event.command == WORKFLOW_COMMAND_PLACE_SENSORS);

    /* A choice made inside a step is kept across rebuilds of that step. */
    state.has_pending = false;
    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_DOWN, 201U);
    build(&snapshot, &state, &ranging, &model);
    assert(model.actions[model.selected_action] == WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    assert(model.actions[model.selected_action] == WORKFLOW_COMMAND_BACK);

    /* The key can arrive before the screen rebuilt for a new step: it must not act on the old choice. */
    ui_workflow_model_state_t late = {0};
    snapshot = make_snapshot(WORKFLOW_STEP_CALIBRATING);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &late, &ranging, &model);
    assert(model.action_count == 1U);      /* no live-signals entry while a calibration runs */
    workflow_snapshot_t result = make_snapshot(WORKFLOW_STEP_CALIB_RESULT);
    result.calibration.result_ok = true;
    result.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK) | WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACE_SENSORS);
    event = ui_workflow_model_handle_key(&result, &late, UI_WORKFLOW_KEY_OK, 300U);
    assert(event.command == WORKFLOW_COMMAND_PLACE_SENSORS);
}

static void set_placement_node(
    workflow_snapshot_t *snapshot,
    size_t index,
    const char *name,
    bool stable,
    workflow_placement_reason_t reason,
    uint8_t samples,
    int32_t range_mm)
{
    workflow_placement_node_t *node = &snapshot->placement.per_node[index];
    (void)snprintf(node->node, sizeof(node->node), "%s", name);
    node->vertical = true;
    node->stable = stable;
    node->reason = reason;
    node->sample_count = samples;
    node->range_mm = range_mm;
    if (index >= snapshot->placement.per_node_count) snapshot->placement.per_node_count = index + 1U;
}

static void test_placement_shows_continue_with_the_blocking_reason_and_live_signals(void)
{
    /* 2026-10-31 bench: "Sensors placed" was simply absent while the nodes were not stable, with no reason. */
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_PLACEMENT);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    set_placement_node(&snapshot, 0U, "MAIN", true, WORKFLOW_PLACEMENT_REASON_STABLE, 30U, 0);
    set_placement_node(&snapshot, 1U, "G1", true, WORKFLOW_PLACEMENT_REASON_STABLE, 4U, 2);
    set_placement_node(&snapshot, 2U, "G2", false, WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED, 9U, 134);
    build(&snapshot, &state, &ranging, &model);

    /* The first node that is not stable is named, with its reason and the number behind it, above the rows. */
    const size_t rows = 3U;
    assert(model.line_count == rows + 1U);
    const ui_workflow_model_line_t *why = &model.lines[0];
    assert(why->key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_BLOCKED);
    assert(strcmp(why->args[0], "G2") == 0);
    assert(why->arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(strcmp(why->args[2], " 14/10cm") == 0);   /* 134 mm over the 100 mm limit, rounded up */
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ROW);

    /* Continue is on the list, ahead of Back, but is not a command the firmware would accept. */
    assert(model.action_count == 2U);
    assert(model.actions[0] == UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED);
    assert(model.action_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_ACTION_PLACEMENT_CONFIRMED);
    assert(model.actions[1] == WORKFLOW_COMMAND_BACK);
    assert((model.actions_mask & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED)) == 0U);
    assert(model.selected_action == 0U);

    /* OK on the disabled Continue sends nothing and says why instead. */
    ui_workflow_event_t event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 500U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    assert(state.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_PLACEMENT_BLOCKED);
    assert(!state.has_pending);

    /* An uncertainty is rounded up: one millimetre over the limit must not read as equal to it. */
    set_placement_node(&snapshot, 2U, "G2", false, WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED, 9U, WORKFLOW_PLACEMENT_MAX_RANGE_MM + 1);
    build(&snapshot, &state, &ranging, &model);
    assert(strcmp(model.lines[0].args[2], " 11/10cm") == 0);

    /* Still collecting: the sample count against the minimum instead of an uncertainty. */
    set_placement_node(&snapshot, 2U, "G2", false, WORKFLOW_PLACEMENT_REASON_COLLECTING, 2U, -1);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].arg_keys[1] == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_REASON_COLLECTING);
    char collecting[16];
    (void)snprintf(collecting, sizeof(collecting), " 2/%u", (unsigned)WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(strcmp(model.lines[0].args[2], collecting) == 0);
    /* A reason with no number leaves that part empty. */
    set_placement_node(&snapshot, 2U, "G2", false, WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL, 0U, -1);
    build(&snapshot, &state, &ranging, &model);
    assert(model.lines[0].args[2][0] == '\0');

    /* Once the firmware allows it, Continue is the real command and OK sends it. */
    ui_workflow_model_state_t ready_state = {0};
    snapshot.allowed_actions |= WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED);
    snapshot.placement.all_stable = true;
    set_placement_node(&snapshot, 2U, "G2", true, WORKFLOW_PLACEMENT_REASON_STABLE, 5U, 3);
    build(&snapshot, &ready_state, &ranging, &model);
    assert(model.action_count == 2U);
    assert(model.actions[0] == WORKFLOW_COMMAND_PLACEMENT_CONFIRMED);
    assert(model.lines[rows].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ALL_STABLE);   /* after the rows, no blocker line */
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ROW);
    event = ui_workflow_model_handle_key(&snapshot, &ready_state, UI_WORKFLOW_KEY_OK, 600U);
    assert(event.type == UI_WORKFLOW_EVENT_SEND_COMMAND);
    assert(event.command == WORKFLOW_COMMAND_PLACEMENT_CONFIRMED);
}

/* The operator asked for 0.1 m: heights are centimetres, rounded to the nearest, halves away from zero. */
static void test_placement_heights_are_shown_in_centimetres(void)
{
    static const struct { int64_t mm; const char *cm; } cases[] = {
        { 0, "0" }, { 4, "0" }, { 5, "1" }, { 14, "1" }, { 15, "2" }, { 120, "12" }, { 1234, "123" },
        { -4, "0" }, { -5, "-1" }, { -14, "-1" }, { -15, "-2" }, { -129, "-13" }, { -1234, "-123" },
    };
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_PLACEMENT);
        snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
        snapshot.placement.all_stable = true;
        set_placement_node(&snapshot, 0U, "G1", true, WORKFLOW_PLACEMENT_REASON_STABLE, 9U, 0);
        snapshot.placement.per_node[0].height_rel_main_mm_valid = true;
        snapshot.placement.per_node[0].height_rel_main_mm = cases[i].mm;
        ui_workflow_model_t model;
        build(&snapshot, &state, &ranging, &model);
        assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ROW);
        assert(strcmp(model.lines[0].args[2], cases[i].cm) == 0);
    }
}

/* Ranging distances, the residual and the sigma are shown in centimetres, rounded to the nearest. */
static void test_ranging_values_are_shown_in_centimetres(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM) | WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    ranging.available = true;
    ranging.pairs_done = 3U;
    ranging.pairs_total = 3U;
    ranging.residual_valid = true;
    ranging.residual_mm = 74;                 /* 7 cm */
    ranging.pair_count = 1U;
    ranging.pairs[0].node_a = 0U;
    ranging.pairs[0].node_b = 1U;
    ranging.pairs[0].valid = true;
    ranging.pairs[0].distance_mm = 2345;      /* 235 cm, half away from zero */
    ranging.node_count = 1U;
    (void)snprintf(ranging.nodes[0], sizeof(ranging.nodes[0]), "G1");
    ranging.sigma_mm[0] = 124;                /* 12 cm */
    build(&snapshot, &state, &ranging, &model);

    bool saw_pairs = false, saw_distance = false, saw_sigma = false;
    for (size_t i = 0U; i < model.line_count; ++i) {
        if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIRS) {
            saw_pairs = true;
            assert(strcmp(model.lines[i].args[2], "7") == 0);
        } else if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_DISTANCE) {
            saw_distance = true;
            assert(strcmp(model.lines[i].args[2], "235") == 0);
        } else if (model.lines[i].key == UI_I18N_KEY_TFT_WORKFLOW_RANGING_SIGMA) {
            saw_sigma = true;
            assert(strcmp(model.lines[i].args[1], "12") == 0);
        }
    }
    assert(saw_pairs && saw_distance && saw_sigma);
}

static void test_live_signals_are_offered_only_before_recording_steps_that_need_them(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
        workflow_snapshot_t snapshot = make_snapshot(step);
        snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
        state = (ui_workflow_model_state_t){0};
        build(&snapshot, &state, &ranging, &model);
        bool offered = false;
        for (size_t i = 0U; i < model.action_count; ++i) {
            if (model.actions[i] == UI_WORKFLOW_ACTION_LIVE_SIGNALS) offered = true;
        }
        /* Only once the BLE Channel Sounding positions exist: not during calibration, placing or ranging. */
        assert(offered == (step == WORKFLOW_STEP_RANGING_RESULT));
    }
}

static void test_live_signals_open_from_the_ranging_result_without_a_workflow_command(void)
{
    ui_workflow_model_state_t state = {0};
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_t model;
    workflow_snapshot_t snapshot = make_snapshot(WORKFLOW_STEP_RANGING_RESULT);
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM) | WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
    build(&snapshot, &state, &ranging, &model);
    assert(model.action_count == 3U);
    assert(model.actions[0] == WORKFLOW_COMMAND_CONFIRM);
    assert(model.actions[1] == UI_WORKFLOW_ACTION_LIVE_SIGNALS);
    assert(model.actions[2] == WORKFLOW_COMMAND_BACK);
    ui_workflow_event_t event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_DOWN, 700U);
    event = ui_workflow_model_handle_key(&snapshot, &state, UI_WORKFLOW_KEY_OK, 701U);
    assert(event.type == UI_WORKFLOW_EVENT_OPEN_LIVE_SIGNALS);
    assert(!state.has_pending);
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
    test_ranging_page_says_what_the_wifi_radio_is_doing();
    test_ranging_manual_entry();
    test_ranging_result_three_outcomes();
    test_ranging_result_displays_pair_values();
    test_all_steps();
    test_selection_does_not_stick_to_back_after_a_step_with_only_back();
    test_placement_shows_continue_with_the_blocking_reason_and_live_signals();
    test_placement_heights_are_shown_in_centimetres();
    test_ranging_values_are_shown_in_centimetres();
    test_live_signals_are_offered_only_before_recording_steps_that_need_them();
    test_live_signals_open_from_the_ranging_result_without_a_workflow_command();
    test_actions_follow_allowed_mask();
    test_async_sequence_wrap_keeps_new_request_pending();
    test_material_save_sequence_wrap_keeps_new_save_pending();
    test_max460_guided_step_content_and_placement_gate();
    test_max460_tft_action_set_matches_core_for_guided_steps();
    test_the_wifi_choice_before_the_distance_measurement_is_explicit();
    test_a_ranging_pair_without_a_distance_shows_both_directions_and_the_cause();
    test_sensor_counts();
    test_remote_slot_layout_is_fixed();
    test_left_behavior_each_step();
    test_right_preview_matrix();
    test_delete_confirmation();
    test_pending_notices();
    test_unknown_values_and_ranging_sigma();
    test_calibration_phase_and_result_status();
    test_calibration_result_points_to_placing_the_geophones_next();
    test_calibration_main_row_has_no_imu_fields();
    test_calibration_reason_mapping();
    test_result_routing_and_distances();
    test_preview_stop_double_ok();
    test_sensor_check_action_and_warning_for_low_remote_counts();
    test_workflow_start_and_preview_regressions();
    test_insufficient_nodes_results_use_existing_warning();
    return 0;
#endif
}
