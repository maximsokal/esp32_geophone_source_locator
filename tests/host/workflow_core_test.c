#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "workflow_core.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

typedef struct {
    workflow_step_t from;
    workflow_command_t command;
    workflow_step_t to;
} transition_t;

static const transition_t allowed_transitions[] = {
    { WORKFLOW_STEP_OBJECT, WORKFLOW_COMMAND_START_MEASUREMENT, WORKFLOW_STEP_SENSOR_CHECK },
    { WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_GO_CALIBRATION, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_OBJECT },
    { WORKFLOW_STEP_CALIB_PREP, WORKFLOW_COMMAND_SENSORS_PLACED, WORKFLOW_STEP_CALIBRATING },
    { WORKFLOW_STEP_CALIB_PREP, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_SENSOR_CHECK },
    { WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_CALIBRATION_DONE, WORKFLOW_STEP_CALIB_RESULT },
    { WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_RETRY, WORKFLOW_STEP_CALIBRATING },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_PLACE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_RESULT },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_REQUEST_GPS, WORKFLOW_STEP_GPS_PENDING },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_CAPTURE_FAILED, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_ACCEPT, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS, WORKFLOW_STEP_RANGING },
    { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_RANGING_DONE, WORKFLOW_STEP_RANGING_RESULT },
    { WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_RETRY, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_CONFIRM, WORKFLOW_STEP_SCAN_CHOICE },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_SET_DISTANCE, WORKFLOW_STEP_RANGING_RESULT },
    { WORKFLOW_STEP_SCAN_CHOICE, WORKFLOW_COMMAND_SCAN_START, WORKFLOW_STEP_SCANNING },
    { WORKFLOW_STEP_SCAN_CHOICE, WORKFLOW_COMMAND_SCAN_SKIP, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_SCAN_CHOICE, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_RANGING_RESULT },
    { WORKFLOW_STEP_SCANNING, WORKFLOW_COMMAND_SCAN_COMPLETE, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_SCANNING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_SCAN_CHOICE },
    { WORKFLOW_STEP_RECORD_READY, WORKFLOW_COMMAND_START_RECORDING, WORKFLOW_STEP_RECORDING },
    { WORKFLOW_STEP_RECORD_READY, WORKFLOW_COMMAND_MOVE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_RECORD_READY, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_SCAN_CHOICE },
    { WORKFLOW_STEP_RECORDING, WORKFLOW_COMMAND_STOP_RECORDING, WORKFLOW_STEP_STOPPED },
    { WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_CALCULATE_SOURCES, WORKFLOW_STEP_CALCULATING },
    { WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_DELETE_RECORDING, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_RECORD_AGAIN, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_MOVE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_CALCULATING, WORKFLOW_COMMAND_CALCULATION_DONE, WORKFLOW_STEP_RESULT },
    { WORKFLOW_STEP_CALCULATING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_STOPPED },
    { WORKFLOW_STEP_RESULT, WORKFLOW_COMMAND_MOVE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_STOPPED },
};

static workflow_step_t expected_step(workflow_step_t step, workflow_command_t command)
{
    for (size_t i = 0U; i < sizeof(allowed_transitions) / sizeof(allowed_transitions[0]); ++i) {
        if (allowed_transitions[i].from == step && allowed_transitions[i].command == command) {
            return allowed_transitions[i].to;
        }
    }
    return WORKFLOW_STEP_COUNT;
}

static void test_exhaustive_transition_table(void)
{
    for (int step_value = 0; step_value < WORKFLOW_STEP_COUNT; ++step_value) {
        for (int command_value = 0; command_value < WORKFLOW_COMMAND_COUNT; ++command_value) {
            const workflow_step_t step = (workflow_step_t)step_value;
            const workflow_command_t command = (workflow_command_t)command_value;
            const workflow_step_t expected = expected_step(step, command);
            workflow_core_t core;
            workflow_core_init(&core, true);
            core.step = step;
            core.generation = 41U;
            core.calibration_ok = command == WORKFLOW_COMMAND_PLACE_SENSORS;
            core.ranging_ok = command == WORKFLOW_COMMAND_CONFIRM;
            workflow_core_set_calc_settings_ready(&core, true);
            workflow_command_params_t params = {
                .has_generation = command == WORKFLOW_COMMAND_SET_DISTANCE,
                .expected_generation = 41U,
                .has_distance = command == WORKFLOW_COMMAND_SET_DISTANCE,
                .node_a = 0U, .node_b = 1U, .distance_mm = 1000,
                .has_ok = true,
                .ok = command == WORKFLOW_COMMAND_SCAN_COMPLETE,
                .calculation_class = WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN,
                .has_location = command == WORKFLOW_COMMAND_GPS_ACCEPT,
                .latitude = -90.0,
                .longitude = 180.0,
                .accuracy_m = 0.0,
            };
            const workflow_command_result_t result = workflow_core_apply(
                &core,
                command,
                &params,
                5U,
                true
            );
            if (expected == WORKFLOW_STEP_COUNT) {
                assert(result == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
                assert(core.step == step);
                assert(core.generation == 41U);
            }
            else {
                assert(result == WORKFLOW_COMMAND_RESULT_OK);
                assert(core.step == expected);
                assert(core.generation == 42U);
            }
        }
    }
}

static void test_calib_result_requires_explicit_place_sensors(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_CALIB_RESULT;
    core.generation = 19U;
    core.calibration_ok = true;

    workflow_snapshot_t snapshot;
    workflow_core_snapshot(&core, 4U, false, &snapshot);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACE_SENSORS)) != 0U);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED)) == 0U);
    assert(core.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(core.generation == 19U);

    const workflow_command_params_t params = {0};
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_PLACE_SENSORS, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_PLACEMENT);
    assert(core.generation == 20U);
}

static void test_calculation_requires_saved_settings(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_STOPPED;
    workflow_command_params_t params = {0};
    workflow_snapshot_t snapshot;

    workflow_core_snapshot(&core, 4U, false, &snapshot);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES)) == 0U);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CALCULATE_SOURCES, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(core.step == WORKFLOW_STEP_STOPPED);
    assert(strcmp(core.reason, "calc_settings_invalid") == 0);

    workflow_core_set_calc_settings_ready(&core, true);
    workflow_core_snapshot(&core, 4U, false, &snapshot);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES)) != 0U);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CALCULATE_SOURCES, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_CALCULATING);
}

static void test_back_from_calibration_result_returns_to_prep(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_CALIB_RESULT;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_CALIB_PREP);
}

static void test_back_from_ranging_result_returns_to_layout_ready(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_RANGING_RESULT;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_LAYOUT_READY);
}

static void test_back_clears_downstream_workflow_choices(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_LAYOUT_READY;
    core.gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
    core.layout_result_valid = true;
    core.layout_ok = true;
    core.scan_choice = WORKFLOW_SCAN_CHOICE_YES;
    core.scan_upload_status = WORKFLOW_SCAN_UPLOAD_VERIFIED;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_GPS_CHOICE);
    assert(core.gps_status == WORKFLOW_GPS_STATUS_NONE);
    assert(!core.layout_result_valid && !core.layout_ok);
    assert(core.scan_choice == WORKFLOW_SCAN_CHOICE_NONE);
    assert(core.scan_upload_status == WORKFLOW_SCAN_UPLOAD_NONE);

    core.step = WORKFLOW_STEP_SCAN_CHOICE;
    core.gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
    core.layout_result_valid = true;
    core.layout_ok = true;
    core.scan_choice = WORKFLOW_SCAN_CHOICE_YES;
    core.scan_upload_status = WORKFLOW_SCAN_UPLOAD_PENDING;
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(core.gps_status == WORKFLOW_GPS_STATUS_ACCEPTED);
    assert(core.layout_result_valid && core.layout_ok);
    assert(core.scan_choice == WORKFLOW_SCAN_CHOICE_NONE);
    assert(core.scan_upload_status == WORKFLOW_SCAN_UPLOAD_NONE);
}

static void test_back_from_result_returns_to_stopped(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_RESULT;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_STOPPED);
}


static void test_go_calibration_initializes_any_connected_count(void)
{
    for (size_t remotes = 0U; remotes <= 3U; ++remotes) {
        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = WORKFLOW_STEP_SENSOR_CHECK;
        workflow_command_params_t params = {0};
        const workflow_command_result_t result = workflow_core_apply(
            &core, WORKFLOW_COMMAND_GO_CALIBRATION, &params, remotes, false);

        assert(result == WORKFLOW_COMMAND_RESULT_OK);
        assert(core.step == WORKFLOW_STEP_CALIB_PREP);
    }
}

static void test_retry_after_failed_calibration_restarts_calibration(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_CALIB_RESULT;
    core.calibration_ok = false;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_RETRY, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_CALIBRATING);
}

static void test_delete_recording_returns_to_record_ready_preserving_setup(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_STOPPED;
    core.calibration_ok = true;
    core.ranging_ok = true;
    workflow_command_params_t params = {0};

    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_DELETE_RECORDING, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_RECORD_READY);
    assert(core.calibration_ok && core.ranging_ok);
}

static void test_back_from_running_steps_records_cancellation_reason(void)
{
    static const struct {
        workflow_step_t from;
        workflow_step_t to;
    } cases[] = {
        { WORKFLOW_STEP_CALIBRATING, WORKFLOW_STEP_CALIB_PREP },
        { WORKFLOW_STEP_RANGING, WORKFLOW_STEP_LAYOUT_READY },
        { WORKFLOW_STEP_CALCULATING, WORKFLOW_STEP_STOPPED },
    };
    workflow_command_params_t params = {0};

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = cases[i].from;
        assert(workflow_core_apply(&core, WORKFLOW_COMMAND_BACK, &params, 4U, false) ==
            WORKFLOW_COMMAND_RESULT_OK);
        assert(core.step == cases[i].to);
        assert(strcmp(core.reason, "cancelled_by_back") == 0);
    }
}

static void test_sensor_count_guards_zero_through_five(void)
{
    for (size_t remotes = 0U; remotes <= 5U; ++remotes) {
        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = WORKFLOW_STEP_SENSOR_CHECK;
        workflow_command_params_t params = {0};
        workflow_snapshot_t snapshot;
        workflow_core_snapshot(&core, remotes, false, &snapshot);
        if (remotes < 3U) {
            assert(snapshot.sensor_sufficiency == WORKFLOW_SENSOR_INSUFFICIENT);
        }
        else {
            const workflow_sensor_sufficiency_t expected = remotes == 3U
                ? WORKFLOW_SENSOR_PLANAR_2D
                : WORKFLOW_SENSOR_CAPABLE_3D;
            assert(snapshot.sensor_sufficiency == expected);
        }
        assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION)) != 0U);
        const workflow_command_result_t result = workflow_core_apply(
            &core,
            WORKFLOW_COMMAND_GO_CALIBRATION,
            &params,
            remotes,
            false);
        assert(result == WORKFLOW_COMMAND_RESULT_OK);
        assert(core.step == WORKFLOW_STEP_CALIB_PREP);
        assert(snapshot.assigned_remotes == remotes);
    }
}

static void test_stale_generation_and_placement_guard(void)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    workflow_command_params_t params = {
        .has_generation = true,
        .expected_generation = 0U,
    };
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_START_MEASUREMENT,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_STALE);
    assert(core.step == WORKFLOW_STEP_OBJECT && core.generation == 1U);

    params.expected_generation = 1U;
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_START_MEASUREMENT,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_OK);
    core.step = WORKFLOW_STEP_PLACEMENT;
    params.has_generation = false;
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_PLACEMENT_CONFIRMED,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(strcmp(core.reason, "placement_not_ready") == 0);
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_PLACEMENT_CONFIRMED,
        &params,
        4U,
        true
    ) == WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_GPS_CHOICE);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_GPS_SKIP, &params, 4U, true) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CALCULATE_POSITIONS, &params, 4U, true) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_RANGING);
}

static void test_success_and_failure_result_routes(void)
{
    workflow_core_t core;
    workflow_command_params_t params = { .has_ok = true, .ok = true };
    workflow_core_init(&core, true);
    core.step = WORKFLOW_STEP_CALIBRATING;
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CALIBRATION_DONE, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_PLACE_SENSORS, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_PLACEMENT);

    core.step = WORKFLOW_STEP_RANGING_RESULT;
    core.ranging_ok = true;
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CONFIRM, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_SCAN_CHOICE);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_SCAN_SKIP, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_RECORD_READY);

    core.step = WORKFLOW_STEP_CALCULATING;
    params.ok = false;
    params.reason = "geometry_degenerate";
    params.calculation_class = WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS;
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_CALCULATION_DONE, &params, 4U, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    workflow_snapshot_t snapshot;
    workflow_core_snapshot(&core, 4U, false, &snapshot);
    assert(snapshot.step == WORKFLOW_STEP_RESULT);
    assert(snapshot.recommended_action == WORKFLOW_COMMAND_MOVE_SENSORS);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS)) != 0U);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_RECORD_AGAIN)) != 0U);
    assert(strcmp(snapshot.reason, "geometry_degenerate") == 0);
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_RECORD_AGAIN,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_OK);
    assert(core.step == WORKFLOW_STEP_RECORD_READY);

    core.step = WORKFLOW_STEP_CALCULATING;
    params.ok = true;
    params.calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    assert(workflow_core_set_progress(&core, 0));
    assert(workflow_core_set_progress(&core, 100));
    assert(!workflow_core_set_progress(&core, 101));
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_CALCULATION_DONE,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_core_snapshot(&core, 4U, false, &snapshot);
    assert(snapshot.progress == WORKFLOW_PROGRESS_UNKNOWN);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_RECORD_AGAIN)) == 0U);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_MOVE_SENSORS)) != 0U);
    assert(workflow_core_apply(
        &core,
        WORKFLOW_COMMAND_RECORD_AGAIN,
        &params,
        4U,
        false
    ) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
}

int main(void)
{
#ifdef MAX449_CASE
#if MAX449_CASE == 5
    test_go_calibration_initializes_any_connected_count();
#else
#error "unknown MAX449_CASE for workflow_core_test"
#endif
#else
    test_exhaustive_transition_table();
    test_calib_result_requires_explicit_place_sensors();
    test_calculation_requires_saved_settings();
    test_back_from_calibration_result_returns_to_prep();
    test_back_from_ranging_result_returns_to_layout_ready();
    test_back_clears_downstream_workflow_choices();
    test_back_from_result_returns_to_stopped();
    test_go_calibration_initializes_any_connected_count();
    test_retry_after_failed_calibration_restarts_calibration();
    test_delete_recording_returns_to_record_ready_preserving_setup();
    test_back_from_running_steps_records_cancellation_reason();
    test_sensor_count_guards_zero_through_five();
    test_stale_generation_and_placement_guard();
    test_success_and_failure_result_routes();
    return 0;
#endif
}
