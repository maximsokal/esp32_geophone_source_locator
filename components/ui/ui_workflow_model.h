#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "object_store.h"
#include "result_view.h"
#include "ui_i18n_catalog.gen.h"
#include "workflow_core.h"

#define UI_WORKFLOW_MODEL_MAX_ACTIONS 14U
#define UI_WORKFLOW_MODEL_MAX_LINES 40U
#define UI_WORKFLOW_MODEL_ARGS 6U
#define UI_WORKFLOW_MODEL_ARG_SIZE 80U

typedef struct {
    bool available;
    size_t pairs_done;
    size_t pairs_total;
    bool residual_valid;
    int32_t residual_mm;
    size_t node_count;
    char nodes[WORKFLOW_MAX_CALIBRATION_NODES][8];
    int32_t sigma_mm[WORKFLOW_MAX_CALIBRATION_NODES];
} ui_workflow_ranging_t;

typedef struct {
    bool has_selected_action;
    workflow_command_t selected_action;
    bool delete_confirm;
    bool has_pending;
    workflow_command_t pending_command;
    uint32_t pending_seq;
    ui_i18n_key_t notice_key;
    uint64_t notice_until_ms;
    uint64_t recording_elapsed_ms;
    bool recording_elapsed_valid;
    char active_object_name[OBJECT_STORE_NAME_SIZE];

    bool result_data_ready;
    bool result_load_failed;
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
} ui_workflow_event_type_t;

typedef struct {
    ui_workflow_event_type_t type;
    workflow_command_t command;
} ui_workflow_event_t;

void ui_workflow_model_build(
    const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging,
    ui_workflow_model_state_t *state,
    uint64_t now_ms,
    ui_workflow_model_t *model);

ui_workflow_event_t ui_workflow_model_handle_key(
    const workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state,
    ui_workflow_key_t key,
    uint64_t now_ms);

void ui_workflow_model_command_sent(
    ui_workflow_model_state_t *state,
    workflow_command_t command,
    esp_err_t result,
    uint32_t seq,
    uint64_t now_ms);

bool ui_workflow_model_preview_ok(
    bool recording,
    bool stop_armed,
    uint64_t now_ms,
    uint64_t *armed_until_ms);
