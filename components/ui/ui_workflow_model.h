#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_store.h"
#include "result_view.h"
#include "ui_i18n_catalog.gen.h"
#include "workflow_core.h"

#define UI_WORKFLOW_MODEL_MAX_ACTIONS 18U
#define UI_WORKFLOW_MODEL_MAX_LINES 40U
#define UI_WORKFLOW_MODEL_ARGS 6U
#define UI_WORKFLOW_MODEL_ARG_SIZE 80U
#define UI_WORKFLOW_REMOTE_SLOT_COUNT 4U
#define UI_WORKFLOW_RANGING_MAX_PAIRS 10U

/*
 * The distance editor works in metres with one decimal: three digits, tens of metres, metres and tenths (10 cm is
 * the precision of a manual entry; the value is still stored and sent in millimetres). It used to show six digits of
 * millimetres with the cursor on the first one, so one press of UP or DOWN made a value like 101000 or 901000 mm,
 * which the cable check rejects (board log 2026-11-10 17:27: nine rejected saves in a row). The cursor starts on
 * the metres digit.
 */
#define UI_WORKFLOW_DISTANCE_DIGITS 3U
#define UI_WORKFLOW_DISTANCE_START_DIGIT 1U
#define UI_WORKFLOW_DISTANCE_STEP_MM 100U
#define UI_WORKFLOW_DISTANCE_MAX_MM 99900U

typedef struct {
    bool visible;
    bool connected;
} ui_workflow_remote_slot_t;

/* State of the Wi-Fi radio around the distance measurement (ranging_radio_state_t, kept free of the component header). */
#define UI_WORKFLOW_RADIO_IDLE 0U
#define UI_WORKFLOW_RADIO_PENDING 1U       /* about to go off: the page says so for a few seconds */
#define UI_WORKFLOW_RADIO_OFF 2U
#define UI_WORKFLOW_RADIO_RESUMING 3U

typedef struct {
    uint8_t radio_state;    /* meaningful also while `available` is false: the page waits for the radio before the nodes start */
    bool available;
    bool partial;
    uint64_t started_at_ms, updated_at_ms;
    size_t pairs_done;
    size_t pairs_total;
    bool residual_valid;
    int32_t residual_mm;
    size_t node_count;
    char nodes[WORKFLOW_MAX_CALIBRATION_NODES][8];
    int32_t sigma_mm[WORKFLOW_MAX_CALIBRATION_NODES];
    size_t pair_count;
    struct {
        uint8_t node_a, node_b;
        bool valid;
        bool cable_exceeded;
        bool manual;
        int32_t distance_mm;
        /* Why the pair has no distance and what each direction measured (nrf_cs_pair_cause_t; 0 = nothing to say). */
        uint8_t cause;
        uint8_t diag_ab_samples, diag_ba_samples;
        int32_t diag_ab_mm, diag_ba_mm;
    } pairs[UI_WORKFLOW_RANGING_MAX_PAIRS];
} ui_workflow_ranging_t;

typedef enum {
    UI_WORKFLOW_MATERIAL_CLOSED = 0,
    UI_WORKFLOW_MATERIAL_SELECT,
    UI_WORKFLOW_MATERIAL_CUSTOM_SPEED,
    UI_WORKFLOW_MATERIAL_CUSTOM_SIGMA,
    UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MIN,
    UI_WORKFLOW_MATERIAL_CUSTOM_RANGE_MAX,
    UI_WORKFLOW_MATERIAL_SAVING,
} ui_workflow_material_view_t;

typedef enum {
    UI_WORKFLOW_DISTANCE_CLOSED = 0,
    UI_WORKFLOW_DISTANCE_PAIR,
    UI_WORKFLOW_DISTANCE_VALUE,
    UI_WORKFLOW_DISTANCE_SAVING,
} ui_workflow_distance_view_t;

typedef struct {
    bool has_selected_action;
    workflow_command_t selected_action;
    /* Step the selection belongs to: a new step starts on its first (forward) action. */
    bool selection_step_valid;
    workflow_step_t selection_step;
    bool delete_confirm;
    bool has_pending;
    workflow_command_t pending_command;
    uint32_t pending_seq;
    ui_i18n_key_t notice_key;
    uint64_t notice_until_ms;
    uint64_t recording_elapsed_ms;
    bool recording_elapsed_valid;
    char active_object_name[OBJECT_STORE_NAME_SIZE];
    ui_workflow_material_view_t material_view;
    size_t material_preset_count;
    size_t material_stored_index;
    size_t material_selection_index;
    bool material_selection_initialized;
    bool material_is_set;
    ui_i18n_key_t material_name_key;
    ui_i18n_key_t material_candidate_name_key;
    uint32_t material_candidate_v_mps;
    uint32_t material_candidate_sigma_mps;
    uint32_t material_custom_v_mps;
    uint32_t material_custom_sigma_mps;
    bool material_custom_range_set;
    uint32_t material_custom_range_min_mps;
    uint32_t material_custom_range_max_mps;
    bool material_custom_values_valid;
    bool material_save_pending;
    uint32_t material_save_seq;
    ui_workflow_distance_view_t distance_view;
    size_t distance_pair;
    uint32_t distance_mm;
    unsigned distance_digit;    /* 0 = tens of metres, 1 = metres, 2 = tenths; see UI_WORKFLOW_DISTANCE_START_DIGIT */
    /* A save was accepted: the editor stays on the pair and follows the new workflow generation and ranging result. */
    bool distance_resync;
    uint32_t distance_generation;
    uint64_t distance_started_at_ms, distance_updated_at_ms;

    bool result_data_ready;
    bool result_load_failed;
    /* The recording sample rate of the next run and its longest recording in seconds (0: none); refreshed by the screen. */
    uint32_t sample_rate_hz;
    uint32_t record_limit_s;
    bool result_waveform_available;
    bool result_source_valid;
    double result_source_m[3];
    object_store_geometry_t result_geometry;
} ui_workflow_model_state_t;

typedef struct {
    ui_i18n_key_t key;
    ui_i18n_key_t arg_keys[UI_WORKFLOW_MODEL_ARGS];
    char args[UI_WORKFLOW_MODEL_ARGS][UI_WORKFLOW_MODEL_ARG_SIZE];
} ui_workflow_model_line_t;

typedef struct {
    ui_i18n_key_t title_key;
    ui_i18n_key_t hint_key;
    ui_i18n_key_t notice_key;
    ui_workflow_model_line_t material_action_line;
    size_t line_count;
    ui_workflow_model_line_t lines[UI_WORKFLOW_MODEL_MAX_LINES];
    size_t action_count;
    uint32_t actions_mask;
    workflow_command_t actions[UI_WORKFLOW_MODEL_MAX_ACTIONS];
    ui_i18n_key_t action_keys[UI_WORKFLOW_MODEL_MAX_ACTIONS];
    size_t selected_action;
    bool pending;
    workflow_command_t pending_command;
    bool show_delete_confirm;
    bool show_3d_hint;
    bool progress_indeterminate;
    bool material_picker_visible;
    bool distance_picker_visible;
} ui_workflow_model_t;

typedef enum {
    UI_WORKFLOW_KEY_UP = 0,
    UI_WORKFLOW_KEY_DOWN,
    UI_WORKFLOW_KEY_LEFT,
    UI_WORKFLOW_KEY_RIGHT,
    UI_WORKFLOW_KEY_OK,
} ui_workflow_key_t;

typedef enum {
    UI_WORKFLOW_EVENT_NONE = 0,
    UI_WORKFLOW_EVENT_SEND_COMMAND,
    UI_WORKFLOW_EVENT_LEAVE_OBJECTS,
    UI_WORKFLOW_EVENT_OPEN_PREVIEW,
    UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED,
    UI_WORKFLOW_EVENT_SAVE_MATERIAL,
    UI_WORKFLOW_EVENT_LEAVE_HOME,
    UI_WORKFLOW_EVENT_EDIT_DISTANCE,
    UI_WORKFLOW_EVENT_SAVE_DISTANCE,
    UI_WORKFLOW_EVENT_OPEN_LIVE_SIGNALS,
    UI_WORKFLOW_EVENT_CYCLE_SAMPLE_RATE,
} ui_workflow_event_type_t;

/*
 * Entries of the action list that are not workflow commands. MATERIAL (WORKFLOW_COMMAND_COUNT) is the
 * material picker; these two follow it.
 *   PLACEMENT_BLOCKED  "Sensors placed" shown while the firmware has not allowed it; OK only explains why.
 *   LIVE_SIGNALS       opens the live geophone view before the recording steps.
 */
#define UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED ((workflow_command_t)(WORKFLOW_COMMAND_COUNT + 1))
#define UI_WORKFLOW_ACTION_LIVE_SIGNALS ((workflow_command_t)(WORKFLOW_COMMAND_COUNT + 2))
/*  SAMPLE_RATE       at RECORD_READY: OK moves to the next recording sample-rate preset (the line above it shows rate and limit). */
#define UI_WORKFLOW_ACTION_SAMPLE_RATE ((workflow_command_t)(WORKFLOW_COMMAND_COUNT + 3))

typedef struct {
    ui_workflow_event_type_t type;
    workflow_command_t command;
    size_t material_index;
    uint32_t custom_v_mps;
    uint32_t custom_sigma_mps;
    bool has_range;
    uint32_t range_min_mps;
    uint32_t range_max_mps;
    workflow_command_params_t distance;
} ui_workflow_event_t;

void ui_workflow_model_build(
    const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging,
    ui_workflow_model_state_t *state,
    uint64_t now_ms,
    ui_workflow_model_t *model);

/* Sensor status changes visibility, never the fixed four-slot layout. */
void ui_workflow_remote_slot_layout(
    uint8_t connected_mask,
    ui_workflow_remote_slot_t slots[UI_WORKFLOW_REMOTE_SLOT_COUNT]);

ui_workflow_event_t ui_workflow_model_handle_key(
    const workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_key_t key,
    uint64_t now_ms);

ui_workflow_event_t ui_workflow_distance_handle_key(
    const workflow_snapshot_t *snapshot, const ui_workflow_ranging_t *ranging,
    ui_workflow_model_state_t *state, ui_workflow_key_t key, uint64_t now_ms);
void ui_workflow_distance_open(const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging, ui_workflow_model_state_t *state);

void ui_workflow_model_command_sent(
    ui_workflow_model_state_t *state,
    workflow_command_t command,
    esp_err_t result,
    uint32_t seq,
    uint64_t now_ms);

void ui_workflow_model_material_save_sent(
    ui_workflow_model_state_t *state,
    esp_err_t result,
    uint32_t seq,
    uint64_t now_ms);

bool ui_workflow_model_preview_ok(
    bool recording,
    bool stop_armed,
    uint64_t now_ms,
    uint64_t *armed_until_ms);
