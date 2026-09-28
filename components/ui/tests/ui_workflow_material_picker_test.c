#include "localization_material_presets.h"
#include "ui_workflow_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static workflow_snapshot_t stopped_snapshot(void)
{
    workflow_snapshot_t snapshot = {0};
    snapshot.step = WORKFLOW_STEP_STOPPED;
    snapshot.progress = WORKFLOW_PROGRESS_UNKNOWN;
    snapshot.calc_settings_ready = true;
    snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES);
    return snapshot;
}

static ui_i18n_key_t material_name_key(const char *id)
{
    if (strcmp(id, "concrete") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CONCRETE;
    if (strcmp(id, "reinforced_concrete") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_REINFORCED_CONCRETE;
    if (strcmp(id, "brick_masonry") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_BRICK_MASONRY;
    if (strcmp(id, "rubble") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_RUBBLE;
    if (strcmp(id, "steel") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_STEEL;
    if (strcmp(id, "soil_dry") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_SOIL_DRY;
    if (strcmp(id, "soil_saturated") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_SOIL_SATURATED;
    if (strcmp(id, "custom") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CUSTOM;
    return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_UNKNOWN;
}

static void set_candidate(ui_workflow_model_state_t *state)
{
    if (state->material_selection_index >= state->material_preset_count) {
        state->material_candidate_name_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CUSTOM;
        state->material_candidate_v_mps = state->material_custom_values_valid
            ? state->material_custom_v_mps : 1000U;
        state->material_candidate_sigma_mps = state->material_custom_values_valid
            ? state->material_custom_sigma_mps : 200U;
        return;
    }
    const localization_material_preset_t *preset =
        localization_material_preset_at(state->material_selection_index);
    assert(preset != NULL);
    state->material_candidate_name_key = material_name_key(preset->id);
    state->material_candidate_v_mps = preset->propagation_speed_mps;
    state->material_candidate_sigma_mps = preset->propagation_speed_uncertainty_mps;
}

static void prepare_state(ui_workflow_model_state_t *state, const char *material_id)
{
    state->material_preset_count = localization_material_preset_count();
    state->material_is_set = material_id[0] != '\0';
    state->material_name_key = material_name_key(material_id);
    state->material_stored_index = state->material_preset_count;
    if (strcmp(material_id, "custom") != 0) {
        for (size_t i = 0U; i < state->material_preset_count; ++i) {
            const localization_material_preset_t *preset = localization_material_preset_at(i);
            if (preset != NULL && strcmp(preset->id, material_id) == 0) {
                state->material_stored_index = i;
                break;
            }
        }
    }
    state->material_selection_index = state->material_stored_index;
    state->material_selection_initialized = true;
    set_candidate(state);
}

static void build(
    workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_model_t *model)
{
    ui_workflow_ranging_t ranging = {0};
    ui_workflow_model_build(snapshot, &ranging, state, 100U, model);
}

static void press(
    workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_model_t *model,
    ui_workflow_key_t key)
{
    const ui_workflow_event_t event = ui_workflow_model_handle_key(snapshot, state, key, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    set_candidate(state);
    build(snapshot, state, model);
}

static void open_picker(
    workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_model_t *model)
{
    build(snapshot, state, model);
    assert(model->action_count > 0U);
    assert(model->actions[0] == (workflow_command_t)WORKFLOW_COMMAND_COUNT);
    const ui_workflow_event_t event = ui_workflow_model_handle_key(
        snapshot, state, UI_WORKFLOW_KEY_OK, 100U);
    assert(event.type == UI_WORKFLOW_EVENT_NONE);
    set_candidate(state);
    build(snapshot, state, model);
    assert(model->material_picker_visible);
}

static void test_row_is_first_and_localized_when_unset_or_set(void)
{
    workflow_snapshot_t snapshot = stopped_snapshot();
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;
    prepare_state(&state, "");
    build(&snapshot, &state, &model);
    assert(model.actions[0] == (workflow_command_t)WORKFLOW_COMMAND_COUNT);
    assert(model.action_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_UNSET);
    assert(model.action_count == 2U);
    assert(model.actions[1] == WORKFLOW_COMMAND_CALCULATE_SOURCES);
    assert((model.actions_mask & WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES)) != 0U);

    prepare_state(&state, "concrete");
    build(&snapshot, &state, &model);
    assert(model.action_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_ROW);
    assert(model.material_action_line.key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_ROW);
    assert(model.material_action_line.arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CONCRETE);
    assert(model.actions[1] == WORKFLOW_COMMAND_CALCULATE_SOURCES);
}

static void test_picker_wraps_and_uses_preset_table(void)
{
    workflow_snapshot_t snapshot = stopped_snapshot();
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;
    prepare_state(&state, "concrete");
    open_picker(&snapshot, &state, &model);
    assert(model.lines[0].key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_OPTION);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CONCRETE);
    const localization_material_preset_t *concrete = localization_material_preset_lookup("concrete");
    assert(concrete != NULL);
    assert(model.lines[1].key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SPEED);
    char expected[12];
    (void)snprintf(expected, sizeof(expected), "%u", (unsigned)concrete->propagation_speed_mps);
    assert(strcmp(model.lines[1].args[0], expected) == 0);
    (void)snprintf(expected, sizeof(expected), "%u", (unsigned)concrete->propagation_speed_uncertainty_mps);
    assert(strcmp(model.lines[1].args[1], expected) == 0);

    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_UP);
    assert(state.material_selection_index == state.material_preset_count);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CUSTOM);
    assert(strcmp(model.lines[1].args[0], "1000") == 0);
    assert(strcmp(model.lines[1].args[1], "200") == 0);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_selection_index == 0U);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CONCRETE);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_selection_index == 1U);
    assert(model.lines[0].arg_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_REINFORCED_CONCRETE);
}

static void test_custom_steps_bounds_and_back_navigation(void)
{
    workflow_snapshot_t snapshot = stopped_snapshot();
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;
    prepare_state(&state, "custom");
    open_picker(&snapshot, &state, &model);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_OK);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CUSTOM_SPEED);
    assert(state.material_custom_v_mps == 1000U && state.material_custom_sigma_mps == 200U);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_UP);
    assert(state.material_custom_v_mps == 1050U);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_custom_v_mps == 1000U);

    state.material_custom_v_mps = 7950U;
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_UP);
    assert(state.material_custom_v_mps == 7950U);
    state.material_custom_v_mps = 150U;
    state.material_custom_sigma_mps = 200U;
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_custom_v_mps == 100U);
    assert(state.material_custom_sigma_mps == 90U);

    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_OK);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_LEFT);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CUSTOM_SPEED);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_OK);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_UP);
    assert(state.material_custom_sigma_mps == 90U);
    for (size_t i = 0U; i < 8U; ++i) press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_custom_sigma_mps == 10U);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    assert(state.material_custom_sigma_mps == 10U);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_LEFT);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CUSTOM_SPEED);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_LEFT);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_SELECT);
    const ui_workflow_event_t cancel = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_LEFT, 100U);
    assert(cancel.type == UI_WORKFLOW_EVENT_NONE);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CLOSED);
}

static void test_pending_save_success_and_failure_notices(void)
{
    workflow_snapshot_t snapshot = stopped_snapshot();
    ui_workflow_model_state_t state = {0};
    ui_workflow_model_t model;
    prepare_state(&state, "concrete");
    open_picker(&snapshot, &state, &model);
    press(&snapshot, &state, &model, UI_WORKFLOW_KEY_DOWN);
    const ui_workflow_event_t save = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_OK, 100U);
    assert(save.type == UI_WORKFLOW_EVENT_SAVE_MATERIAL);
    assert(save.material_index == 1U);
    ui_workflow_model_material_save_sent(&state, ESP_OK, 4U, 100U);
    build(&snapshot, &state, &model);
    assert(state.material_save_pending);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVING);
    assert(model.material_picker_visible);
    const ui_workflow_event_t ignored = ui_workflow_model_handle_key(
        &snapshot, &state, UI_WORKFLOW_KEY_DOWN, 101U);
    assert(ignored.type == UI_WORKFLOW_EVENT_NONE);
    snapshot.last_material_save.seq = 3U;
    build(&snapshot, &state, &model);
    assert(state.material_save_pending);

    snapshot.last_material_save.seq = 4U;
    snapshot.last_material_save.result = ESP_FAIL;
    build(&snapshot, &state, &model);
    assert(!state.material_save_pending);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_SELECT);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVE_FAILED);

    ui_workflow_model_material_save_sent(&state, ESP_OK, 5U, 200U);
    snapshot.last_material_save.seq = 5U;
    snapshot.last_material_save.result = ESP_OK;
    build(&snapshot, &state, &model);
    assert(!state.material_save_pending);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_CLOSED);
    assert(state.material_stored_index == 1U);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVED);
    assert(model.action_keys[0] == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_ROW);

    ui_workflow_model_material_save_sent(&state, ESP_ERR_NO_MEM, 0U, 300U);
    build(&snapshot, &state, &model);
    assert(model.notice_key == UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_SAVE_FAILED);
    assert(state.material_view == UI_WORKFLOW_MATERIAL_SELECT);
}

int main(void)
{
    test_row_is_first_and_localized_when_unset_or_set();
    test_picker_wraps_and_uses_preset_table();
    test_custom_steps_bounds_and_back_navigation();
    test_pending_save_success_and_failure_notices();
    return 0;
}
