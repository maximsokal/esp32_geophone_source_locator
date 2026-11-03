#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "workflow_core.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

typedef struct {
    workflow_step_t from;
    workflow_command_t command;
    workflow_step_t to;
} model_transition_t;

static const model_transition_t s_transitions[] = {
    { WORKFLOW_STEP_OBJECT, WORKFLOW_COMMAND_START_MEASUREMENT, WORKFLOW_STEP_SENSOR_CHECK },
    { WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_GO_CALIBRATION, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_OBJECT },
    { WORKFLOW_STEP_CALIB_PREP, WORKFLOW_COMMAND_SENSORS_PLACED, WORKFLOW_STEP_CALIBRATING },
    { WORKFLOW_STEP_CALIB_PREP, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_SENSOR_CHECK },
    { WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_CALIBRATION_DONE, WORKFLOW_STEP_CALIB_RESULT },
    { WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_RETRY, WORKFLOW_STEP_CALIBRATING },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_PLACE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_PREP },
    { WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_CALIB_RESULT },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_REQUEST_GPS, WORKFLOW_STEP_GPS_PENDING },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_ACCEPT, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_CAPTURE_FAILED, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS, WORKFLOW_STEP_RANGING },
    { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI, WORKFLOW_STEP_RANGING },
    { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_GPS_CHOICE },
    { WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_RANGING_DONE, WORKFLOW_STEP_RANGING_RESULT },
    { WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_RETRY, WORKFLOW_STEP_LAYOUT_READY },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_CONFIRM, WORKFLOW_STEP_SCAN_CHOICE },
    { WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_LAYOUT_READY },
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
    { WORKFLOW_STEP_RESULT, WORKFLOW_COMMAND_RECORD_AGAIN, WORKFLOW_STEP_RECORD_READY },
    { WORKFLOW_STEP_RESULT, WORKFLOW_COMMAND_MOVE_SENSORS, WORKFLOW_STEP_PLACEMENT },
    { WORKFLOW_STEP_RESULT, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_STOPPED },
};

typedef struct {
    workflow_step_t step;
    uint32_t generation;
    workflow_sensor_sufficiency_t sensor_sufficiency;
    uint8_t assigned_remotes;
    bool calibration_ok;
    bool ranging_ok;
    bool ranging_keep_wifi;
    bool calc_settings_ready;
    workflow_calculation_class_t calculation_class;
    workflow_gps_status_t gps_status;
    bool layout_result_valid;
    bool layout_ok;
    workflow_scan_choice_t scan_choice;
    workflow_scan_upload_status_t scan_upload_status;
    int progress;
} model_state_t;

typedef struct {
    workflow_command_t command;
    bool has_generation;
    uint8_t generation_mode; /* 0: absent, 1: match, 2: stale */
    bool has_ok;
    bool ok;
    workflow_calculation_class_t calculation_class;
    bool has_location;
    double latitude;
    double longitude;
    double accuracy_m;
    bool altitude_valid;
    double altitude_m;
    double altitude_accuracy_m;
    size_t assigned_remotes;
    bool placement_ready;
} action_t;

static workflow_step_t model_transition(workflow_step_t step, workflow_command_t command)
{
    for (size_t i = 0U; i < sizeof(s_transitions) / sizeof(s_transitions[0]); ++i) {
        if (s_transitions[i].from == step && s_transitions[i].command == command) {
            return s_transitions[i].to;
        }
    }
    return WORKFLOW_STEP_COUNT;
}

static bool model_location_valid(const workflow_command_params_t *params)
{
    return params->has_location && isfinite(params->latitude) &&
        params->latitude >= -90.0 && params->latitude <= 90.0 &&
        isfinite(params->longitude) && params->longitude >= -180.0 && params->longitude <= 180.0 &&
        isfinite(params->accuracy_m) && params->accuracy_m >= 0.0 &&
        params->accuracy_m <= 3.402823466e+38 &&
        (!params->altitude_valid || (isfinite(params->altitude_m) &&
            isfinite(params->altitude_accuracy_m) && params->altitude_accuracy_m >= 0.0 &&
            params->altitude_accuracy_m <= 3.402823466e+38));
}

static void model_advance_generation(model_state_t *state)
{
    ++state->generation;
    if (state->generation == 0U) ++state->generation;
}

static workflow_command_result_t model_apply(
    model_state_t *state,
    workflow_command_t command,
    const workflow_command_params_t *params,
    size_t assigned_remotes,
    bool placement_ready)
{
    if (command < 0 || command >= WORKFLOW_COMMAND_COUNT) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }
    state->assigned_remotes = assigned_remotes > UINT8_MAX ? UINT8_MAX : (uint8_t)assigned_remotes;
    state->sensor_sufficiency = assigned_remotes < 3U ? WORKFLOW_SENSOR_INSUFFICIENT
        : assigned_remotes == 3U ? WORKFLOW_SENSOR_PLANAR_2D : WORKFLOW_SENSOR_CAPABLE_3D;
    if (params->has_generation && params->expected_generation != state->generation) {
        return WORKFLOW_COMMAND_RESULT_STALE;
    }

    workflow_step_t next = model_transition(state->step, command);
    if ((command == WORKFLOW_COMMAND_PLACEMENT_CONFIRMED && state->step == WORKFLOW_STEP_PLACEMENT) ||
        ((command == WORKFLOW_COMMAND_CALCULATE_POSITIONS || command == WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI) &&
         state->step == WORKFLOW_STEP_LAYOUT_READY)) {
        if (!placement_ready) return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    }
    if (command == WORKFLOW_COMMAND_CALCULATE_SOURCES && state->step == WORKFLOW_STEP_STOPPED &&
        !state->calc_settings_ready) return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;

    switch (command) {
        case WORKFLOW_COMMAND_CALIBRATION_DONE:
            if (state->step == WORKFLOW_STEP_CALIBRATING) {
                if (!params->has_ok) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                state->calibration_ok = params->ok;
            }
            break;
        case WORKFLOW_COMMAND_RETRY:
            if (state->step == WORKFLOW_STEP_CALIB_RESULT) {
                if (state->calibration_ok) next = WORKFLOW_STEP_COUNT;
            } else if (state->step == WORKFLOW_STEP_RANGING_RESULT) {
                if (state->ranging_ok) next = WORKFLOW_STEP_COUNT;
                else state->layout_result_valid = state->layout_ok = false;
            }
            break;
        case WORKFLOW_COMMAND_PLACE_SENSORS:
            if (state->step == WORKFLOW_STEP_CALIB_RESULT && state->calibration_ok) {
                state->gps_status = WORKFLOW_GPS_STATUS_NONE;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            } else if (state->step == WORKFLOW_STEP_CALIB_RESULT) next = WORKFLOW_STEP_COUNT;
            break;
        case WORKFLOW_COMMAND_PLACEMENT_CONFIRMED:
            if (state->step == WORKFLOW_STEP_PLACEMENT && placement_ready) {
                state->gps_status = WORKFLOW_GPS_STATUS_NONE;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_REQUEST_GPS:
            if (state->step == WORKFLOW_STEP_GPS_CHOICE) state->gps_status = WORKFLOW_GPS_STATUS_PENDING;
            break;
        case WORKFLOW_COMMAND_GPS_ACCEPT:
            if (state->step == WORKFLOW_STEP_GPS_PENDING) {
                if (!model_location_valid(params)) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                state->gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_GPS_CAPTURE_FAILED:
            if (state->step == WORKFLOW_STEP_GPS_PENDING) state->gps_status = WORKFLOW_GPS_STATUS_FAILED;
            break;
        case WORKFLOW_COMMAND_GPS_SKIP:
            if (state->step == WORKFLOW_STEP_GPS_CHOICE || state->step == WORKFLOW_STEP_GPS_PENDING) {
                state->gps_status = WORKFLOW_GPS_STATUS_SKIPPED;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_CALCULATE_POSITIONS:
        case WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI:
            if (state->step == WORKFLOW_STEP_LAYOUT_READY) {
                state->layout_result_valid = state->layout_ok = false;
                state->ranging_keep_wifi = command == WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI;
            }
            break;
        case WORKFLOW_COMMAND_RANGING_DONE:
            if (state->step == WORKFLOW_STEP_RANGING) {
                if (!params->has_ok) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                state->ranging_ok = params->ok;
                state->layout_result_valid = true;
                state->layout_ok = params->ok;
            }
            break;
        case WORKFLOW_COMMAND_CONFIRM:
            if (state->step == WORKFLOW_STEP_RANGING_RESULT && state->ranging_ok) {
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            } else if (state->step == WORKFLOW_STEP_RANGING_RESULT) next = WORKFLOW_STEP_COUNT;
            break;
        case WORKFLOW_COMMAND_SCAN_START:
            if (state->step == WORKFLOW_STEP_SCAN_CHOICE) {
                state->scan_choice = WORKFLOW_SCAN_CHOICE_YES;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_PENDING;
            }
            break;
        case WORKFLOW_COMMAND_SCAN_SKIP:
            if (state->step == WORKFLOW_STEP_SCAN_CHOICE) {
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NO;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_SCAN_COMPLETE:
            if (state->step == WORKFLOW_STEP_SCANNING) {
                if (!params->has_ok || !params->ok) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_VERIFIED;
            }
            break;
        case WORKFLOW_COMMAND_MOVE_SENSORS:
            if (state->step == WORKFLOW_STEP_RECORD_READY || state->step == WORKFLOW_STEP_STOPPED ||
                state->step == WORKFLOW_STEP_RESULT) {
                state->gps_status = WORKFLOW_GPS_STATUS_NONE;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_CALCULATION_DONE:
            if (state->step == WORKFLOW_STEP_CALCULATING) {
                if (!params->has_ok) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                if (!params->ok && params->calculation_class != WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN &&
                    params->calculation_class != WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS &&
                    params->calculation_class != WORKFLOW_CALCULATION_CLASS_INTERNAL) {
                    return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                }
            }
            break;
        case WORKFLOW_COMMAND_RECORD_AGAIN:
            if (state->step == WORKFLOW_STEP_RESULT &&
                state->calculation_class == WORKFLOW_CALCULATION_CLASS_NONE) next = WORKFLOW_STEP_COUNT;
            break;
        case WORKFLOW_COMMAND_BACK:
            if (state->step == WORKFLOW_STEP_GPS_CHOICE || state->step == WORKFLOW_STEP_GPS_PENDING ||
                state->step == WORKFLOW_STEP_LAYOUT_READY) {
                state->gps_status = WORKFLOW_GPS_STATUS_NONE;
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            } else if (state->step == WORKFLOW_STEP_RANGING || state->step == WORKFLOW_STEP_RANGING_RESULT) {
                state->layout_result_valid = state->layout_ok = false;
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            } else if (state->step == WORKFLOW_STEP_SCAN_CHOICE || state->step == WORKFLOW_STEP_SCANNING ||
                state->step == WORKFLOW_STEP_RECORD_READY) {
                state->scan_choice = WORKFLOW_SCAN_CHOICE_NONE;
                state->scan_upload_status = WORKFLOW_SCAN_UPLOAD_NONE;
            }
            break;
        case WORKFLOW_COMMAND_SET_DISTANCE:
            if (state->step == WORKFLOW_STEP_RANGING_RESULT) {
                if (!params->has_generation || !params->has_distance || !params->has_ok ||
                    params->distance_mm <= 0 || params->node_a == params->node_b)
                    return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
                state->ranging_ok = params->ok;
                state->layout_result_valid = true;
                state->layout_ok = params->ok;
                model_advance_generation(state);
                return WORKFLOW_COMMAND_RESULT_OK;
            }
            break;
        default:
            break;
    }

    if (next == WORKFLOW_STEP_COUNT) return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    state->step = next;
    state->progress = WORKFLOW_PROGRESS_UNKNOWN;
    state->calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    model_advance_generation(state);
    if (command == WORKFLOW_COMMAND_CALCULATION_DONE) {
        state->calculation_class = params->ok
            ? WORKFLOW_CALCULATION_CLASS_NONE : params->calculation_class;
    }
    return WORKFLOW_COMMAND_RESULT_OK;
}

static model_state_t model_from_core(const workflow_core_t *core)
{
    return (model_state_t){
        .step = core->step,
        .generation = core->generation,
        .sensor_sufficiency = core->sensor_sufficiency,
        .assigned_remotes = core->assigned_remotes,
        .calibration_ok = core->calibration_ok,
        .ranging_ok = core->ranging_ok,
        .ranging_keep_wifi = core->ranging_keep_wifi,
        .calc_settings_ready = core->calc_settings_ready,
        .calculation_class = core->calculation_class,
        .gps_status = core->gps_status,
        .layout_result_valid = core->layout_result_valid,
        .layout_ok = core->layout_ok,
        .scan_choice = core->scan_choice,
        .scan_upload_status = core->scan_upload_status,
        .progress = core->progress,
    };
}

static bool core_matches(const workflow_core_t *core, const model_state_t *model)
{
    return core->step == model->step && core->generation == model->generation &&
        core->sensor_sufficiency == model->sensor_sufficiency &&
        core->assigned_remotes == model->assigned_remotes &&
        core->calibration_ok == model->calibration_ok && core->ranging_ok == model->ranging_ok &&
        core->ranging_keep_wifi == model->ranging_keep_wifi &&
        core->calc_settings_ready == model->calc_settings_ready &&
        core->calculation_class == model->calculation_class && core->gps_status == model->gps_status &&
        core->layout_result_valid == model->layout_result_valid && core->layout_ok == model->layout_ok &&
        core->scan_choice == model->scan_choice && core->scan_upload_status == model->scan_upload_status &&
        core->progress == model->progress;
}

static void assert_matches(const workflow_core_t *core, const model_state_t *model)
{
    assert(core_matches(core, model));
}

static void seed_state(workflow_core_t *core, model_state_t *model, workflow_step_t step)
{
    workflow_core_init(core, true);
    core->step = step;
    core->generation = 41U;
    core->progress = 23;
    core->calibration_ok = true;
    core->ranging_ok = true;
    core->calc_settings_ready = true;
    core->calculation_class = WORKFLOW_CALCULATION_CLASS_INTERNAL;
    core->gps_status = WORKFLOW_GPS_STATUS_ACCEPTED;
    core->layout_result_valid = true;
    core->layout_ok = true;
    core->scan_choice = WORKFLOW_SCAN_CHOICE_YES;
    core->scan_upload_status = WORKFLOW_SCAN_UPLOAD_VERIFIED;
    *model = model_from_core(core);
}

static workflow_command_params_t permissive_params(void)
{
    return (workflow_command_params_t){
        .has_ok = true,
        .ok = true,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN,
        .has_location = true,
        .latitude = -90.0,
        .longitude = 180.0,
        .accuracy_m = 0.0,
    };
}

static void apply_case(
    workflow_step_t step,
    workflow_command_t command,
    workflow_command_params_t params,
    size_t remotes,
    bool placement_ready,
    workflow_command_result_t expected_result,
    workflow_step_t expected_step)
{
    workflow_core_t core;
    model_state_t model;
    seed_state(&core, &model, step);
    const workflow_command_result_t model_result = model_apply(&model, command, &params, remotes, placement_ready);
    const workflow_command_result_t actual = workflow_core_apply(&core, command, &params, remotes, placement_ready);
    assert(model_result == expected_result);
    assert(actual == expected_result);
    assert(model.step == expected_step);
    assert_matches(&core, &model);
}

static void apply_case_with_flags(
    workflow_step_t step,
    workflow_command_t command,
    workflow_command_params_t params,
    size_t remotes,
    bool placement_ready,
    bool calibration_ok,
    bool ranging_ok,
    bool calc_settings_ready,
    workflow_command_result_t expected_result,
    workflow_step_t expected_step)
{
    workflow_core_t core;
    model_state_t model;
    seed_state(&core, &model, step);
    core.calibration_ok = model.calibration_ok = calibration_ok;
    core.ranging_ok = model.ranging_ok = ranging_ok;
    core.calc_settings_ready = model.calc_settings_ready = calc_settings_ready;
    const workflow_command_result_t model_result = model_apply(&model, command, &params, remotes, placement_ready);
    const workflow_command_result_t actual = workflow_core_apply(&core, command, &params, remotes, placement_ready);
    assert(model_result == expected_result);
    assert(actual == expected_result);
    assert(model.step == expected_step);
    assert_matches(&core, &model);
}

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static action_t random_action(uint32_t *state)
{
    static const double latitudes[] = { -90.0, 90.0, -90.01, 90.01, 0.0, NAN };
    static const double longitudes[] = { -180.0, 180.0, -180.01, 180.01, 0.0, INFINITY };
    static const double accuracies[] = { 0.0, 1.0, -0.01, NAN };
    const uint32_t value = random_next(state);
    const uint32_t second = random_next(state);
    return (action_t){
        .command = (workflow_command_t)(value % WORKFLOW_COMMAND_COUNT),
        .has_generation = (value & 1U) != 0U,
        .generation_mode = (uint8_t)((value >> 1) % 3U),
        .has_ok = (value & 4U) != 0U,
        .ok = (value & 8U) != 0U,
        .calculation_class = (workflow_calculation_class_t)((value >> 4) % 4U),
        .has_location = (value & 16U) != 0U,
        .latitude = latitudes[(second >> 1) % (sizeof(latitudes) / sizeof(latitudes[0]))],
        .longitude = longitudes[(second >> 5) % (sizeof(longitudes) / sizeof(longitudes[0]))],
        .accuracy_m = accuracies[(second >> 9) % (sizeof(accuracies) / sizeof(accuracies[0]))],
        .altitude_valid = (second & 1U) != 0U,
        .altitude_m = (double)(int32_t)(second % 10000U),
        .altitude_accuracy_m = (second & 2U) != 0U ? 1.0 : -1.0,
        .assigned_remotes = (size_t)((second >> 12) % 10U),
        .placement_ready = (second & 4U) != 0U,
    };
}

static workflow_command_params_t params_for_action(const action_t *action, const model_state_t *state)
{
    uint32_t expected_generation = state->generation;
    if (action->generation_mode == 2U) expected_generation ^= UINT32_C(0x80000000);
    return (workflow_command_params_t){
        .has_generation = action->has_generation,
        .expected_generation = expected_generation,
        .has_ok = action->has_ok,
        .ok = action->ok,
        .calculation_class = action->calculation_class,
        .has_location = action->has_location,
        .latitude = action->latitude,
        .longitude = action->longitude,
        .accuracy_m = action->accuracy_m,
        .altitude_valid = action->altitude_valid,
        .altitude_m = action->altitude_m,
        .altitude_accuracy_m = action->altitude_accuracy_m,
    };
}

static bool replay(const action_t *actions, size_t count, uint32_t seed, bool report)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    model_state_t model = model_from_core(&core);
    for (size_t i = 0U; i < count; ++i) {
        const workflow_command_params_t params = params_for_action(&actions[i], &model);
        const workflow_command_result_t expected = model_apply(
            &model, actions[i].command, &params, actions[i].assigned_remotes, actions[i].placement_ready);
        const workflow_command_result_t actual = workflow_core_apply(
            &core, actions[i].command, &params, actions[i].assigned_remotes, actions[i].placement_ready);
        if (actual != expected || !core_matches(&core, &model)) {
            if (report) {
                fprintf(stderr, "seed=%08x mismatch at %zu cmd=%d expected=%d actual=%d step=%d/%d gen=%u/%u\n",
                    seed, i, (int)actions[i].command, (int)expected, (int)actual,
                    (int)model.step, (int)core.step, model.generation, core.generation);
            }
            return false;
        }
    }
    return true;
}

static void print_sequence(const action_t *actions, size_t count)
{
    fprintf(stderr, "shrunk_sequence[%zu] = {", count);
    for (size_t i = 0U; i < count; ++i) {
        fprintf(stderr, "%s%d", i == 0U ? "" : ",", (int)actions[i].command);
    }
    fprintf(stderr, "}\n");
}

static void shrink_sequence(action_t *actions, size_t *count, uint32_t seed)
{
    for (size_t chunk = *count / 2U; chunk > 0U;) {
        bool reduced = false;
        for (size_t start = 0U; start + chunk <= *count; ++start) {
            action_t candidate[256];
            const size_t candidate_count = *count - chunk;
            memcpy(candidate, actions, start * sizeof(candidate[0]));
            memcpy(&candidate[start], &actions[start + chunk], (*count - start - chunk) * sizeof(candidate[0]));
            if (!replay(candidate, candidate_count, seed, false)) {
                memcpy(actions, candidate, candidate_count * sizeof(actions[0]));
                *count = candidate_count;
                reduced = true;
                break;
            }
        }
        if (!reduced) chunk /= 2U;
    }
}

static void assert_sequence_matches(action_t *actions, size_t count, uint32_t seed)
{
    if (replay(actions, count, seed, false)) return;
    shrink_sequence(actions, &count, seed);
    (void)replay(actions, count, seed, true);
    print_sequence(actions, count);
    assert(false && "workflow reference model mismatch");
}

/* MAX-491 mutation-only: independent oracle covers every state/command pair. */
static void test_all_state_command_pairs(void)
{
    assert(WORKFLOW_STEP_COUNT == 19);
    assert(WORKFLOW_COMMAND_COUNT == 27);
    for (int state = 0; state < WORKFLOW_STEP_COUNT; ++state) {
        for (int command = 0; command < WORKFLOW_COMMAND_COUNT; ++command) {
            workflow_core_t core;
            model_state_t model;
            seed_state(&core, &model, (workflow_step_t)state);
            workflow_command_params_t params = permissive_params();
            const workflow_command_result_t expected = model_apply(
                &model, (workflow_command_t)command, &params, 4U, true);
            const workflow_command_result_t actual = workflow_core_apply(
                &core, (workflow_command_t)command, &params, 4U, true);
            assert(actual == expected);
            assert_matches(&core, &model);
        }
    }
}

/* MAX-491 mutation-only: each production guard is exercised on both sides. */
static void test_guard_boundaries_and_generation(void)
{
    workflow_command_params_t params = permissive_params();
    apply_case(WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_GO_CALIBRATION, params, 2U, false,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_CALIB_PREP);
    apply_case(WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_GO_CALIBRATION, params, 3U, false,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_CALIB_PREP);
    apply_case(WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, params, 4U, false,
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_PLACEMENT);
    apply_case(WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_GPS_CHOICE);
    apply_case(WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS, params, 4U, false,
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_LAYOUT_READY);
    apply_case(WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_RANGING);
    apply_case(WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI, params, 4U, false,
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_LAYOUT_READY);
    apply_case(WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS_KEEP_WIFI, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_RANGING);
    apply_case_with_flags(WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_RETRY, params, 4U, true,
        true, true, true, WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_CALIB_RESULT);
    apply_case_with_flags(WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_RETRY, params, 4U, true,
        false, true, true, WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_CALIBRATING);
    apply_case_with_flags(WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_PLACE_SENSORS, params, 4U, true,
        true, true, true, WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_PLACEMENT);
    apply_case_with_flags(WORKFLOW_STEP_CALIB_RESULT, WORKFLOW_COMMAND_PLACE_SENSORS, params, 4U, true,
        false, true, true, WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_CALIB_RESULT);
    apply_case_with_flags(WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_RETRY, params, 4U, true,
        true, true, true, WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_RANGING_RESULT);
    apply_case_with_flags(WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_RETRY, params, 4U, true,
        true, false, true, WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_LAYOUT_READY);
    apply_case_with_flags(WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_CONFIRM, params, 4U, true,
        true, true, true, WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_SCAN_CHOICE);
    apply_case_with_flags(WORKFLOW_STEP_RANGING_RESULT, WORKFLOW_COMMAND_CONFIRM, params, 4U, true,
        true, false, true, WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_RANGING_RESULT);
    apply_case_with_flags(WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_CALCULATE_SOURCES, params, 4U, true,
        true, true, false, WORKFLOW_COMMAND_RESULT_NOT_ALLOWED, WORKFLOW_STEP_STOPPED);
    apply_case_with_flags(WORKFLOW_STEP_STOPPED, WORKFLOW_COMMAND_CALCULATE_SOURCES, params, 4U, true,
        true, true, true, WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_CALCULATING);

    params.has_ok = false;
    apply_case(WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_CALIBRATION_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_CALIBRATING);
    apply_case(WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_RANGING_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_RANGING);
    apply_case(WORKFLOW_STEP_SCANNING, WORKFLOW_COMMAND_SCAN_COMPLETE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_SCANNING);
    apply_case(WORKFLOW_STEP_CALCULATING, WORKFLOW_COMMAND_CALCULATION_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_CALCULATING);

    params = permissive_params();
    params.ok = false;
    apply_case(WORKFLOW_STEP_CALIBRATING, WORKFLOW_COMMAND_CALIBRATION_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_CALIB_RESULT);
    apply_case(WORKFLOW_STEP_RANGING, WORKFLOW_COMMAND_RANGING_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_RANGING_RESULT);
    apply_case(WORKFLOW_STEP_SCANNING, WORKFLOW_COMMAND_SCAN_COMPLETE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_SCANNING);
    params.calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    apply_case(WORKFLOW_STEP_CALCULATING, WORKFLOW_COMMAND_CALCULATION_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_CALCULATING);
    params.calculation_class = WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS;
    apply_case(WORKFLOW_STEP_CALCULATING, WORKFLOW_COMMAND_CALCULATION_DONE, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_RESULT);

    params = permissive_params();
    params.latitude = 90.001;
    apply_case(WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_ACCEPT, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT, WORKFLOW_STEP_GPS_PENDING);
    params.latitude = -90.0;
    params.longitude = -180.0;
    params.accuracy_m = 0.0;
    apply_case(WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_ACCEPT, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_LAYOUT_READY);

    params = permissive_params();
    params.has_generation = true;
    params.expected_generation = 40U;
    apply_case(WORKFLOW_STEP_OBJECT, WORKFLOW_COMMAND_START_MEASUREMENT, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_STALE, WORKFLOW_STEP_OBJECT);
    params.expected_generation = 41U;
    apply_case(WORKFLOW_STEP_OBJECT, WORKFLOW_COMMAND_START_MEASUREMENT, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_SENSOR_CHECK);
    params.has_generation = false; /* BACK stays valid for generation-less HTTP clients. */
    apply_case(WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_COMMAND_BACK, params, 4U, true,
        WORKFLOW_COMMAND_RESULT_OK, WORKFLOW_STEP_OBJECT);

    workflow_core_t core;
    model_state_t model;
    seed_state(&core, &model, WORKFLOW_STEP_OBJECT);
    core.generation = UINT32_MAX;
    model.generation = UINT32_MAX;
    params = permissive_params();
    assert(model_apply(&model, WORKFLOW_COMMAND_START_MEASUREMENT, &params, 4U, true) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(workflow_core_apply(&core, WORKFLOW_COMMAND_START_MEASUREMENT, &params, 4U, true) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(core.generation == 1U && model.generation == 1U);
    assert_matches(&core, &model);
}

/* MAX-491 fail-before(real): seeded sequences compare every event and shrink any mismatch. */
static void test_seeded_action_sequences(void)
{
    static const uint32_t seeds[] = { UINT32_C(0x4912026), UINT32_C(0x5eed491), UINT32_C(0xc0ffee), 1U };
    for (size_t s = 0U; s < sizeof(seeds) / sizeof(seeds[0]); ++s) {
        uint32_t random = seeds[s];
        action_t actions[256];
        for (size_t i = 0U; i < sizeof(actions) / sizeof(actions[0]); ++i) {
            actions[i] = random_action(&random);
        }
        assert_sequence_matches(actions, sizeof(actions) / sizeof(actions[0]), seeds[s]);
    }
}

int main(void)
{
    test_all_state_command_pairs();
    test_guard_boundaries_and_generation();
    test_seeded_action_sequences();
    puts("workflow FSM model tests passed");
    return 0;
}
