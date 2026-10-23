#include "ui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_task_config.h"
#include "device_status.h"
#include "hw_inventory.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "localization_pick_record.h"
#include "localization_material_presets.h"
#include "localization_run_record.h"
#include "localization_run_store.h"
#include "nrf_topology_service.h"
#include "result_view.h"
#include "seismic_recorder.h"
#include "ui_measurements_internal.h"
#include "ui_measurement_trace.h"
#include "ui_preview3d.h"
#include "ui_utf8_format.h"
#include "ui_workflow_model.h"
#include "workflow_service.h"

#define UI_WORKFLOW_REFRESH_PERIOD_MS 250U
#define UI_WORKFLOW_VISIBLE_ACTIONS 2U
#define UI_WORKFLOW_ACTION_ROW_HEIGHT UI_MENU_ROW_HEIGHT
#define UI_WORKFLOW_ACTION_ROW_STEP UI_MENU_ROW_STEP
#define UI_WORKFLOW_LINE_HEIGHT 20
#define UI_WORKFLOW_LINE_TOP 36
#define UI_WORKFLOW_RESULT_POINTS UI_MEASUREMENT_TRACE_POINTS

typedef struct {
    workflow_step_t rendered_step;
    bool rendered_material_picker;
    size_t rendered_action_count;
    char material_object_id[OBJECT_STORE_ID_SIZE];
    bool snapshot_valid;
    bool workflow_service_ready;
    uint64_t last_snapshot_ms;
    uint64_t recording_started_ms;
    workflow_step_t previous_step;
    workflow_snapshot_t snapshot;
    nrf_topology_ranging_progress_t ranging_snapshot;
    ui_workflow_ranging_t ranging;
    device_status_snapshot_t remote_status_snapshot;
    pod_state_t remote_pod_states[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    ui_workflow_remote_slot_t remote_slots[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    ui_workflow_model_state_t model_state;
    ui_workflow_model_t model;
    ui_i18n_key_t preflight_adc;
    ui_i18n_key_t preflight_barometer;
    ui_i18n_key_t preflight_power;
    char preflight_rail_v[12];

    lv_obj_t *screen;
    lv_obj_t *line_labels[UI_WORKFLOW_MODEL_MAX_LINES];
    size_t line_slots;
    lv_obj_t *remote_slot_cards[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    lv_obj_t *remote_slot_labels[UI_WORKFLOW_REMOTE_SLOT_COUNT];
    bool remote_slot_styles_dirty;
    lv_obj_t *action_rows[UI_WORKFLOW_VISIBLE_ACTIONS];
    lv_obj_t *action_labels[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_valid[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_visible[UI_WORKFLOW_VISIBLE_ACTIONS];
    bool action_row_cache_selected[UI_WORKFLOW_VISIBLE_ACTIONS];
    int action_row_cache_y[UI_WORKFLOW_VISIBLE_ACTIONS];
    lv_obj_t *progress_bar;
    lv_obj_t *progress_spinner;
    lv_obj_t *arrival_markers[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
    bool result_widgets_created;
    bool result_widget_data_applied;
    bool result_load_pending;
    uint32_t result_generation;
    bool result_source_valid;
    double result_source_m[3];
    object_store_geometry_t result_geometry;
    bool result_waveform_available;
    size_t result_point_count;
    seismic_recorder_waveform_point_t result_points[UI_WORKFLOW_RESULT_POINTS];
    bool result_arrival_valid[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
    size_t result_arrival_point[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
    lv_coord_t result_min[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT][UI_WORKFLOW_RESULT_POINTS];
    lv_coord_t result_max[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT][UI_WORKFLOW_RESULT_POINTS];
} ui_workflow_screen_state_t;

_Static_assert(
    UI_WORKFLOW_REMOTE_SLOT_COUNT == DEVICE_STATUS_POD_COUNT,
    "Workflow remote slots must match device status pod slots");

typedef struct {
    uint32_t generation;
    char object_id[OBJECT_STORE_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} ui_workflow_load_request_t;

typedef struct {
    bool failed;
    bool waveform_available;
    bool source_valid;
    double source_m[3];
    object_store_geometry_t geometry;
    size_t point_count;
    seismic_recorder_waveform_point_t points[UI_WORKFLOW_RESULT_POINTS];
    bool arrival_valid[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
    size_t arrival_point[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT];
} ui_workflow_load_result_t;

static ui_workflow_screen_state_t *s_workflow;
static TaskHandle_t s_result_loader_task;
static portMUX_TYPE s_result_request_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_workflow_load_request_t s_result_request;
static bool s_result_request_pending;
static uint32_t s_result_generation;

static ui_i18n_key_t ui_workflow_material_name_key(const char *material_id)
{
    if (strcmp(material_id, "concrete") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CONCRETE;
    if (strcmp(material_id, "reinforced_concrete") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_REINFORCED_CONCRETE;
    if (strcmp(material_id, "brick_masonry") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_BRICK_MASONRY;
    if (strcmp(material_id, "rubble") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_RUBBLE;
    if (strcmp(material_id, "steel") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_STEEL;
    if (strcmp(material_id, "soil_dry") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_SOIL_DRY;
    if (strcmp(material_id, "soil_saturated") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_SOIL_SATURATED;
    if (strcmp(material_id, "custom") == 0) return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CUSTOM;
    return UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_UNKNOWN;
}

static void ui_workflow_prepare_material_model(ui_workflow_screen_state_t *state)
{
    ui_workflow_model_state_t *model = &state->model_state;
    const char *object_id = state->snapshot.object_id;
    if (strcmp(state->material_object_id, object_id) != 0) {
        (void)snprintf(state->material_object_id, sizeof(state->material_object_id), "%s", object_id);
        model->material_view = UI_WORKFLOW_MATERIAL_CLOSED;
        model->material_selection_initialized = false;
        model->material_custom_values_valid = false;
        model->material_custom_range_set = false;
        model->material_custom_range_min_mps = 0U;
        model->material_custom_range_max_mps = 0U;
        model->material_save_pending = false;
    }
    model->material_preset_count = localization_material_preset_count();
    model->material_is_set = state->snapshot.calc_material[0] != '\0';
    model->material_name_key = ui_workflow_material_name_key(state->snapshot.calc_material);
    if (!model->material_selection_initialized) {
        model->material_stored_index = 0U;
        if (strcmp(state->snapshot.calc_material, "custom") == 0) {
            model->material_stored_index = model->material_preset_count;
        } else {
            for (size_t i = 0U; i < model->material_preset_count; ++i) {
                const localization_material_preset_t *preset = localization_material_preset_at(i);
                if (preset != NULL && strcmp(preset->id, state->snapshot.calc_material) == 0) {
                    model->material_stored_index = i;
                    break;
                }
            }
        }
        model->material_selection_index = model->material_stored_index;
        model->material_selection_initialized = true;
    }

    if (model->material_selection_index >= model->material_preset_count) {
        model->material_selection_index = model->material_preset_count;
        model->material_candidate_name_key = UI_I18N_KEY_TFT_WORKFLOW_MATERIAL_NAME_CUSTOM;
        model->material_candidate_v_mps = model->material_custom_values_valid
            ? model->material_custom_v_mps : 1000U;
        model->material_candidate_sigma_mps = model->material_custom_values_valid
            ? model->material_custom_sigma_mps : 200U;
    } else {
        const localization_material_preset_t *preset =
            localization_material_preset_at(model->material_selection_index);
        if (preset != NULL) {
            model->material_candidate_name_key = ui_workflow_material_name_key(preset->id);
            model->material_candidate_v_mps = preset->propagation_speed_mps;
            model->material_candidate_sigma_mps = preset->propagation_speed_uncertainty_mps;
        }
    }
}

static void ui_workflow_set_label(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) return;
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) lv_label_set_text(label, text);
}

static void ui_workflow_format_line(
    const ui_workflow_model_line_t *line,
    char *text,
    size_t text_size)
{
    if (text_size == 0U) return;
    text[0] = '\0';
    if (line == NULL) return;
    const char *args[UI_WORKFLOW_MODEL_ARGS];
    for (size_t i = 0U; i < UI_WORKFLOW_MODEL_ARGS; ++i) {
        args[i] = line->arg_keys[i] == UI_I18N_KEY_COUNT
            ? line->args[i] : ui_tr(line->arg_keys[i]);
    }
    (void)ui_utf8_format(text, text_size, ui_tr(line->key),
        args[0], args[1], args[2], args[3], args[4], args[5]);
}

static size_t ui_workflow_visible_actions(const workflow_snapshot_t *snapshot, const ui_workflow_model_t *model)
{
    /* The result needs two readable chart rows above a full-size action. */
    const size_t capacity = snapshot->step == WORKFLOW_STEP_RESULT &&
        snapshot->calculation_class == WORKFLOW_CALCULATION_CLASS_NONE ? 1U : UI_WORKFLOW_VISIBLE_ACTIONS;
    return model->action_count < capacity ? model->action_count : capacity;
}

static size_t ui_workflow_line_slots(const workflow_snapshot_t *snapshot, const ui_workflow_model_t *model)
{
    size_t slots;
    switch (snapshot->step) {
        case WORKFLOW_STEP_CALIBRATING:
        case WORKFLOW_STEP_PLACEMENT:
        case WORKFLOW_STEP_RANGING:
        case WORKFLOW_STEP_RANGING_RESULT:
            slots = 10U;
            break;
        case WORKFLOW_STEP_CALIB_RESULT:
            slots = 10U;
            break;
        case WORKFLOW_STEP_RESULT:
            slots = snapshot->calculation_class == WORKFLOW_CALCULATION_CLASS_NONE ? 2U : 4U;
            break;
        case WORKFLOW_STEP_CALCULATING:
            slots = model->progress_indeterminate ? 0U : 1U;
            break;
        default:
            slots = 4U;
            break;
    }
    const size_t actions = ui_workflow_visible_actions(snapshot, model);
    const int extra_line = snapshot->step == WORKFLOW_STEP_RANGING ? UI_WORKFLOW_LINE_HEIGHT : 0;
    const int available = 214 - (int)actions * UI_WORKFLOW_ACTION_ROW_STEP -
        UI_WORKFLOW_LINE_TOP - extra_line - 4;
    const size_t fitting = available > 0 ? (size_t)(available / UI_WORKFLOW_LINE_HEIGHT) : 0U;
    if (slots > model->line_count) slots = model->line_count;
    return slots < fitting ? slots : fitting;
}

static ui_workflow_load_result_t *ui_workflow_load_result_alloc(void)
{
    return heap_caps_calloc(1U, sizeof(ui_workflow_load_result_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t ui_workflow_load_session_result(
    const ui_workflow_load_request_t *request,
    ui_workflow_load_result_t *result)
{
    seismic_recorder_session_summary_t *sessions = heap_caps_calloc(
        SEISMIC_RECORDER_MAX_SESSIONS, sizeof(*sessions), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sessions == NULL) return ESP_ERR_NO_MEM;
    size_t session_count = 0U;
    esp_err_t err = seismic_recorder_list_sessions(
        request->object_id, sessions, SEISMIC_RECORDER_MAX_SESSIONS, &session_count);
    if (err != ESP_OK) {
        heap_caps_free(sessions);
        return err;
    }

    const seismic_recorder_session_summary_t *session = NULL;
    for (size_t i = 0U; i < session_count; ++i) {
        if (strcmp(sessions[i].session_id, request->session_id) == 0) {
            session = &sessions[i];
            break;
        }
    }
    if (session == NULL || session->sample_rate_hz == 0U || session->samples_written == 0U ||
        session->samples_written > SIZE_MAX) {
        heap_caps_free(sessions);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t frame_count = (size_t)session->samples_written;
    const uint32_t sample_rate_hz = session->sample_rate_hz;
    heap_caps_free(sessions);

    localization_pick_record_t *pick_records = heap_caps_calloc(
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, sizeof(*pick_records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    localization_run_record_t *run_records = heap_caps_calloc(
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, sizeof(*run_records), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pick_records == NULL || run_records == NULL) {
        if (pick_records != NULL) heap_caps_free(pick_records);
        if (run_records != NULL) heap_caps_free(run_records);
        return ESP_ERR_NO_MEM;
    }

    size_t pick_count = 0U;
    const esp_err_t picks_err = localization_run_store_list_picks(
        request->object_id, request->session_id, pick_records,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &pick_count);
    size_t run_count = 0U;
    const esp_err_t runs_err = localization_run_store_list(
        request->object_id, request->session_id, run_records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &run_count);
    if ((picks_err != ESP_OK && picks_err != ESP_ERR_NOT_FOUND) ||
        (runs_err != ESP_OK && runs_err != ESP_ERR_NOT_FOUND)) {
        heap_caps_free(pick_records);
        heap_caps_free(run_records);
        return picks_err != ESP_OK && picks_err != ESP_ERR_NOT_FOUND ? picks_err : runs_err;
    }

    const localization_pick_record_t *latest_picks = NULL;
    for (size_t i = 0U; i < pick_count; ++i) {
        if (latest_picks == NULL || pick_records[i].computed_at_ms > latest_picks->computed_at_ms) {
            latest_picks = &pick_records[i];
        }
    }
    const localization_run_record_t *source = NULL;
    for (size_t i = 0U; i < run_count; ++i) {
        const localization_run_record_t *candidate = &run_records[i];
        if (!localization_run_record_is_primary_candidate(candidate) ||
            (latest_picks != NULL && candidate->computed_at_ms != latest_picks->computed_at_ms)) continue;
        if (source == NULL || candidate->confidence_score > source->confidence_score) source = candidate;
    }
    if (source == NULL && latest_picks != NULL) {
        for (size_t i = 0U; i < run_count; ++i) {
            const localization_run_record_t *candidate = &run_records[i];
            if (localization_run_record_is_primary_candidate(candidate) &&
                (source == NULL || candidate->computed_at_ms > source->computed_at_ms)) source = candidate;
        }
    }
    if (source != NULL) {
        result->source_valid = true;
        result->source_m[0] = source->source_position.x_m;
        result->source_m[1] = source->source_position.y_m;
        result->source_m[2] = source->source_position.z_m;
    }
    (void)seismic_recorder_read_session_geometry(
        request->object_id, request->session_id, &result->geometry);

    if (latest_picks != NULL) {
        geophone_channel_pick_t picks[LOCALIZATION_PICK_RECORD_CHANNEL_COUNT];
        for (size_t i = 0U; i < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++i) {
            picks[i] = latest_picks->channels[i].pick;
        }
        result_view_window_t window = {0};
        const result_view_status_t window_status = result_view_segment_window(
            picks, LOCALIZATION_PICK_RECORD_CHANNEL_COUNT, sample_rate_hz, frame_count,
            RESULT_VIEW_DEFAULT_PRE_S, RESULT_VIEW_DEFAULT_POST_S, &window);
        if (window_status == RESULT_VIEW_OK && window.available) {
            seismic_recorder_waveform_info_t info = {0};
            const esp_err_t waveform_err = seismic_recorder_read_session(
                request->object_id, request->session_id, window.start_frame,
                window.frame_count, UI_WORKFLOW_RESULT_POINTS, result->points,
                UI_WORKFLOW_RESULT_POINTS, &info);
            if (waveform_err == ESP_OK && info.point_count <= UI_WORKFLOW_RESULT_POINTS) {
                result->waveform_available = true;
                result->point_count = info.point_count;
                for (size_t channel = 0U; channel < LOCALIZATION_PICK_RECORD_CHANNEL_COUNT; ++channel) {
                    size_t offset = 0U;
                    if (result_view_arrival_marker_offset(&picks[channel], &window, &offset)) {
                        result->arrival_valid[channel] = true;
                        const size_t denominator = window.frame_count > 1U ? window.frame_count - 1U : 1U;
                        result->arrival_point[channel] = result->point_count > 1U
                            ? offset * (result->point_count - 1U) / denominator : 0U;
                    }
                }
            }
        }
    }
    heap_caps_free(pick_records);
    heap_caps_free(run_records);
    return ESP_OK;
}

static void ui_workflow_result_loader_task(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            ui_workflow_load_request_t request;
            bool have_request = false;
            portENTER_CRITICAL(&s_result_request_mux);
            if (s_result_request_pending) {
                request = s_result_request;
                s_result_request_pending = false;
                have_request = true;
            }
            portEXIT_CRITICAL(&s_result_request_mux);
            if (!have_request) break;

            ui_workflow_load_result_t *result = ui_workflow_load_result_alloc();
            const esp_err_t err = result != NULL
                ? ui_workflow_load_session_result(&request, result) : ESP_ERR_NO_MEM;
            if (result != NULL && err != ESP_OK) result->failed = true;
            bool locked = lvgl_port_lock(0);
            while (!locked) {
                vTaskDelay(pdMS_TO_TICKS(10));
                locked = lvgl_port_lock(50);
            }
            {
                if (s_workflow != NULL && s_workflow->result_generation == request.generation) {
                    ui_workflow_screen_state_t *state = s_workflow;
                    state->model_state.result_data_ready = true;
                    state->model_state.result_load_failed = result == NULL || result->failed;
                    state->model_state.result_waveform_available = result != NULL && result->waveform_available;
                    state->model_state.result_source_valid = result != NULL && result->source_valid;
                    state->result_source_valid = result != NULL && result->source_valid;
                    state->result_waveform_available = result != NULL && result->waveform_available;
                    if (result != NULL) {
                        memcpy(state->model_state.result_source_m, result->source_m, sizeof(result->source_m));
                        state->model_state.result_geometry = result->geometry;
                        memcpy(state->result_source_m, result->source_m, sizeof(result->source_m));
                        state->result_geometry = result->geometry;
                        state->result_point_count = result->point_count;
                        memcpy(state->result_points, result->points, sizeof(result->points));
                        memcpy(state->result_arrival_valid, result->arrival_valid, sizeof(result->arrival_valid));
                        memcpy(state->result_arrival_point, result->arrival_point, sizeof(result->arrival_point));
                    } else {
                        memset(state->model_state.result_source_m, 0, sizeof(state->model_state.result_source_m));
                        memset(&state->model_state.result_geometry, 0, sizeof(state->model_state.result_geometry));
                        memset(state->result_points, 0, sizeof(state->result_points));
                        memset(state->result_arrival_valid, 0, sizeof(state->result_arrival_valid));
                        state->result_point_count = 0U;
                    }
                    state->result_widget_data_applied = false;
                    state->result_load_pending = false;
                }
                lvgl_port_unlock();
            }
            if (result != NULL) heap_caps_free(result);
        }
    }
}

static bool ui_workflow_queue_result_load(ui_workflow_screen_state_t *state)
{
    if (state->snapshot.object_id[0] == '\0' || state->snapshot.session_id[0] == '\0') return false;
    if (s_result_loader_task == NULL) {
        const BaseType_t created = xTaskCreatePinnedToCore(
            ui_workflow_result_loader_task,
            "workflow-result",
            APP_TASK_STACK_OBJECT_RECOVERY,
            NULL,
            APP_TASK_PRIORITY_OBJECT_RECOVERY,
            &s_result_loader_task,
            APP_TASK_CORE_SERVICES);
        if (created != pdPASS) return false;
    }
    if (++s_result_generation == 0U) ++s_result_generation;
    state->result_generation = s_result_generation;
    state->result_load_pending = true;
    ui_workflow_load_request_t request = {.generation = state->result_generation};
    (void)snprintf(request.object_id, sizeof(request.object_id), "%s", state->snapshot.object_id);
    (void)snprintf(request.session_id, sizeof(request.session_id), "%s", state->snapshot.session_id);
    portENTER_CRITICAL(&s_result_request_mux);
    s_result_request = request;
    s_result_request_pending = true;
    portEXIT_CRITICAL(&s_result_request_mux);
    (void)xTaskNotifyGive(s_result_loader_task);
    return true;
}

static void ui_workflow_create_result_markers(ui_workflow_screen_state_t *state)
{
    ui_context_t *context = ui_context();
    for (size_t i = 0U; i < SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT; ++i) {
        lv_obj_t *chart = context->measurement_charts[i];
        lv_obj_t *card = chart != NULL ? lv_obj_get_parent(chart) : NULL;
        if (card == NULL) continue;
        lv_obj_t *marker = lv_obj_create(card);
        if (marker == NULL) continue;
        lv_obj_set_size(marker, 2, 14);
        lv_obj_set_pos(marker, 4, 21);
        lv_obj_set_style_bg_color(marker, lv_color_hex(UI_COLOR_WARNING), 0);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(marker, 0, 0);
        lv_obj_set_style_radius(marker, 0, 0);
        lv_obj_set_hidden(marker, true);
        state->arrival_markers[i] = marker;
    }
}

static void ui_workflow_apply_result_waveform(ui_workflow_screen_state_t *state)
{
    if (state->result_widget_data_applied || !state->model_state.result_data_ready ||
        !state->result_waveform_available) return;
    ui_context_t *context = ui_context();
    float max_abs = 0.0f;
    for (size_t point = 0U; point < state->result_point_count; ++point) {
        for (size_t channel = 0U; channel < SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT; ++channel) {
            const float abs_min = fabsf(state->result_points[point].min_uv[channel]);
            const float abs_max = fabsf(state->result_points[point].max_uv[channel]);
            if (abs_min > max_abs) max_abs = abs_min;
            if (abs_max > max_abs) max_abs = abs_max;
        }
    }
    const int32_t range = max_abs < 1000.0f ? 1000 : (int32_t)(max_abs * 1.25f);
    for (size_t channel = 0U; channel < SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT; ++channel) {
        for (size_t point = 0U; point < UI_WORKFLOW_RESULT_POINTS; ++point) {
            lv_coord_t minimum = 0;
            lv_coord_t maximum = 0;
            if (point < state->result_point_count) {
                minimum = (lv_coord_t)lrintf(state->result_points[point].min_uv[channel]);
                maximum = (lv_coord_t)lrintf(state->result_points[point].max_uv[channel]);
            }
            state->result_min[channel][point] = minimum;
            state->result_max[channel][point] = maximum;
        }
        lv_obj_t *chart = context->measurement_charts[channel];
        lv_chart_series_t *minimum = context->measurement_min_series[channel];
        lv_chart_series_t *maximum = context->measurement_max_series[channel];
        if (chart != NULL && minimum != NULL && maximum != NULL) {
            lv_chart_set_series_ext_y_array(chart, minimum, state->result_min[channel]);
            lv_chart_set_series_ext_y_array(chart, maximum, state->result_max[channel]);
            lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, -range, range);
            lv_obj_invalidate(chart);
        }
        if (state->arrival_markers[channel] != NULL) {
            if (state->result_arrival_valid[channel] && state->result_point_count > 1U) {
                const size_t x = state->result_arrival_point[channel] * 140U / (state->result_point_count - 1U);
                lv_obj_set_pos(state->arrival_markers[channel], 4 + (int)x, 21);
                lv_obj_set_hidden(state->arrival_markers[channel], false);
            } else {
                lv_obj_set_hidden(state->arrival_markers[channel], true);
            }
        }
    }
    state->result_widget_data_applied = true;
}

static void ui_workflow_build_ranging_view(ui_workflow_screen_state_t *state)
{
    memset(&state->ranging, 0, sizeof(state->ranging));
    if ((state->snapshot.step != WORKFLOW_STEP_RANGING &&
         state->snapshot.step != WORKFLOW_STEP_RANGING_RESULT) ||
        nrf_topology_service_ranging_get_progress(&state->ranging_snapshot) != ESP_OK) return;
    const nrf_topology_ranging_progress_t *progress = &state->ranging_snapshot;
    state->ranging.available = true;
    state->ranging.pairs_done = progress->pairs_done;
    state->ranging.pairs_total = progress->pairs_total;
    state->ranging.residual_valid = progress->solution_residual_mm != INT32_MAX;
    state->ranging.residual_mm = progress->solution_residual_mm;
    state->ranging.node_count = progress->geometry.node_count;
    if (state->ranging.node_count > WORKFLOW_MAX_CALIBRATION_NODES) {
        state->ranging.node_count = WORKFLOW_MAX_CALIBRATION_NODES;
    }
    for (size_t i = 0U; i < state->ranging.node_count; ++i) {
        const nrf_cs_node_id_t node_id = progress->geometry.nodes[i].node_id;
        if (node_id == NRF_TOPOLOGY_MAIN_NODE_ID) {
            (void)snprintf(state->ranging.nodes[i], sizeof(state->ranging.nodes[i]), "MAIN");
        } else {
            (void)snprintf(state->ranging.nodes[i], sizeof(state->ranging.nodes[i]), "G%u", (unsigned)node_id);
        }
        state->ranging.sigma_mm[i] = progress->geometry.nodes[i].uncertainty_mm;
    }
}

static void ui_workflow_create_action_slots(ui_workflow_screen_state_t *state)
{
    for (size_t i = 0U; i < UI_WORKFLOW_VISIBLE_ACTIONS; ++i) {
        state->action_rows[i] = ui_create_card_locked(state->screen, 10, 214, 300, UI_WORKFLOW_ACTION_ROW_HEIGHT);
        if (state->action_rows[i] == NULL) continue;
        state->action_labels[i] = ui_create_label_locked(
            state->action_rows[i], 10, UI_MENU_LABEL_Y, 280, "", UI_COLOR_TEXT);
        lv_obj_set_hidden(state->action_rows[i], true);
    }
}

static uint32_t ui_workflow_remote_pod_color(pod_state_t state)
{
    switch (state) {
        case POD_STATE_READY:
            return UI_COLOR_GOOD;
        case POD_STATE_WARNING:
            return UI_COLOR_WARNING;
        case POD_STATE_ERROR:
            return UI_COLOR_ERROR;
        case POD_STATE_DISCONNECTED:
        default:
            return UI_COLOR_MUTED;
    }
}

static void ui_workflow_refresh_remote_slots(ui_workflow_screen_state_t *state)
{
    const bool status_available =
        device_status_try_get_snapshot(&state->remote_status_snapshot) == ESP_OK;
    /* A failed snapshot read must not keep the previous sensor health green. */
    if (!status_available) memset(&state->remote_status_snapshot, 0, sizeof(state->remote_status_snapshot));
    hw_module_info_t adc = {0};
    (void)hw_inventory_get(HW_MODULE_AD7606, &adc);
    state->preflight_adc = adc.state == HW_STATE_PRESENT ? UI_I18N_KEY_TFT_WORKFLOW_SENSOR_OK
        : adc.state == HW_STATE_UNKNOWN ? UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN
        : UI_I18N_KEY_TFT_WORKFLOW_STATE_SENSOR_INVALID;
    const device_status_snapshot_t *device = &state->remote_status_snapshot;
    state->preflight_barometer = device->main.bmp280_valid ? UI_I18N_KEY_TFT_WORKFLOW_SENSOR_OK
        : device->main.bmp280_present ? UI_I18N_KEY_TFT_WORKFLOW_STATE_SENSOR_INVALID
        : UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
    state->preflight_power = device->power.valid ? UI_I18N_KEY_TFT_WORKFLOW_SENSOR_OK
        : UI_I18N_KEY_TFT_WORKFLOW_VALUE_UNKNOWN;
    if (device->power.channels[2].valid) {
        (void)snprintf(state->preflight_rail_v, sizeof(state->preflight_rail_v),
            "%.1f%s", (double)device->power.channels[2].voltage_v,
            device->power.channels[2].overcurrent_latched ||
                device->power.channels[2].overcurrent == DEVICE_POWER_OVERCURRENT_CRITICAL ? "!" : "");
    } else {
        (void)snprintf(state->preflight_rail_v, sizeof(state->preflight_rail_v), "--");
    }
    bool changed = false;
    uint8_t connected_mask = 0U;
    for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
        {
            const pod_state_t pod_state = device->pods[i].state;
            if (state->remote_pod_states[i] != pod_state) changed = true;
            state->remote_pod_states[i] = pod_state;
        }
        const pod_state_t pod_state = state->remote_pod_states[i];
        if (pod_state == POD_STATE_READY || pod_state == POD_STATE_WARNING ||
            pod_state == POD_STATE_ERROR) {
            connected_mask |= (uint8_t)(1U << i);
        }
    }
    ui_workflow_remote_slot_layout(connected_mask, state->remote_slots);
    state->remote_slot_styles_dirty |= changed || state->snapshot.step == WORKFLOW_STEP_SENSOR_CHECK;
}

static bool ui_workflow_remote_slots_visible(const ui_workflow_screen_state_t *state)
{
    if (state->model.line_count > 4U) return false;
    const size_t actions = ui_workflow_visible_actions(&state->snapshot, &state->model);
    const int grid_bottom = UI_WORKFLOW_LINE_TOP +
        (int)state->line_slots * UI_WORKFLOW_LINE_HEIGHT + 2 +
        UI_MENU_ROW_STEP + UI_MENU_ROW_HEIGHT;
    if (grid_bottom >= 214 - (int)actions * UI_WORKFLOW_ACTION_ROW_STEP) return false;
    switch (state->snapshot.step) {
        case WORKFLOW_STEP_OBJECT:
        case WORKFLOW_STEP_SENSOR_CHECK:
        case WORKFLOW_STEP_CALIB_PREP:
        case WORKFLOW_STEP_CALIBRATING:
        case WORKFLOW_STEP_CALIB_RESULT:
        case WORKFLOW_STEP_PLACEMENT:
            return true;
        default:
            return false;
    }
}

static void ui_workflow_create_remote_slots(ui_workflow_screen_state_t *state)
{
    static const ui_i18n_key_t slot_name_keys[UI_WORKFLOW_REMOTE_SLOT_COUNT] = {
        UI_I18N_KEY_MEASUREMENT_NODE_G1,
        UI_I18N_KEY_MEASUREMENT_NODE_G2,
        UI_I18N_KEY_MEASUREMENT_NODE_G3,
        UI_I18N_KEY_MEASUREMENT_NODE_G4,
    };
    if (!ui_workflow_remote_slots_visible(state)) return;

    const int top = UI_WORKFLOW_LINE_TOP +
        (int)(state->line_slots * UI_WORKFLOW_LINE_HEIGHT) + 2;
    for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
        const int x = 10 + (int)(i % 2U) * 154;
        const int y = top + (int)(i / 2U) * UI_MENU_ROW_STEP;
        lv_obj_t *card = ui_create_card_locked(state->screen, x, y, 146, UI_MENU_ROW_HEIGHT);
        if (card == NULL) continue;
        state->remote_slot_cards[i] = card;
        state->remote_slot_labels[i] = ui_create_label_locked(
            card, 8, UI_MENU_LABEL_Y, 130, ui_tr(slot_name_keys[i]), UI_COLOR_MUTED);
    }
    state->remote_slot_styles_dirty = true;
}

static void ui_workflow_update_remote_slots(ui_workflow_screen_state_t *state)
{
    if (!state->remote_slot_styles_dirty) return;
    for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
        lv_obj_t *card = state->remote_slot_cards[i];
        lv_obj_t *label = state->remote_slot_labels[i];
        if (card == NULL || label == NULL) continue;
        if (state->snapshot.step == WORKFLOW_STEP_SENSOR_CHECK) {
            const device_pod_status_t *pod = &state->remote_status_snapshot.pods[i];
            const device_sensor_health_t *health = &pod->sensor_health;
            const bool current = health->current && state->remote_slots[i].connected;
            const char *imu = !current ? "?" : health->imu_valid ? "+" : health->imu_fault ? "!" : "?";
            const char *baro = !current ? "?" : health->barometer_valid ? "+" : health->barometer_fault ? "!" : "?";
            const char *calibration = current && health->imu_calibrated &&
                pod->height_reference.state == DEVICE_HEIGHT_REFERENCE_VALID ? "+" : "-";
            char node[8], text[64];
            (void)snprintf(node, sizeof(node), "G%u", (unsigned)i + 1U);
            (void)ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_TFT_WORKFLOW_SENSOR_NODE),
                node, imu, baro, calibration);
            ui_workflow_set_label(label, text);
        }
        const uint32_t color = ui_workflow_remote_pod_color(state->remote_pod_states[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(color), 0);
        lv_obj_set_style_border_width(card,
            state->remote_slots[i].connected ? 2 : 1, 0);
    }
    state->remote_slot_styles_dirty = false;
}

static void ui_workflow_render_step_locked(ui_workflow_screen_state_t *state)
{
    const char *footer = state->model.hint_key < UI_I18N_KEY_COUNT
        ? ui_tr(state->model.hint_key) : ui_tr(UI_I18N_KEY_TFT_WORKFLOW_HINT_NAVIGATION);
    state->screen = ui_create_screen_base_locked(ui_tr(state->model.title_key), footer);
    if (state->screen == NULL) return;
    state->rendered_step = state->snapshot.step;
    state->rendered_material_picker = state->model.material_picker_visible;
    state->rendered_action_count = state->model.action_count;
    state->line_slots = ui_workflow_line_slots(&state->snapshot, &state->model);
    memset(state->line_labels, 0, sizeof(state->line_labels));
    memset(state->remote_slot_cards, 0, sizeof(state->remote_slot_cards));
    memset(state->remote_slot_labels, 0, sizeof(state->remote_slot_labels));
    memset(state->action_rows, 0, sizeof(state->action_rows));
    memset(state->action_labels, 0, sizeof(state->action_labels));
    memset(state->action_row_cache_valid, 0, sizeof(state->action_row_cache_valid));
    memset(state->action_row_cache_visible, 0, sizeof(state->action_row_cache_visible));
    memset(state->action_row_cache_selected, 0, sizeof(state->action_row_cache_selected));
    memset(state->action_row_cache_y, 0, sizeof(state->action_row_cache_y));
    memset(state->arrival_markers, 0, sizeof(state->arrival_markers));
    state->progress_bar = NULL;
    state->progress_spinner = NULL;
    state->result_widgets_created = false;
    state->result_widget_data_applied = false;

    if (state->snapshot.step == WORKFLOW_STEP_RECORDING) {
        ui_measurements_create_charts_locked(state->screen, 68);
    } else if (state->snapshot.step == WORKFLOW_STEP_RESULT &&
        state->snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_NONE) {
        ui_measurements_create_charts_locked(state->screen, 78);
        ui_workflow_create_result_markers(state);
        state->result_widgets_created = true;
    } else if (state->snapshot.step == WORKFLOW_STEP_RANGING) {
        state->progress_bar = lv_bar_create(state->screen);
        if (state->progress_bar != NULL) {
            lv_obj_set_pos(state->progress_bar, 20, UI_WORKFLOW_LINE_TOP + UI_WORKFLOW_LINE_HEIGHT);
            lv_obj_set_size(state->progress_bar, 280, 8);
            lv_bar_set_range(state->progress_bar, 0, 100);
            lv_obj_set_hidden(state->progress_bar, true);
        }
    }

    for (size_t i = 0U; i < state->line_slots; ++i) {
            state->line_labels[i] = ui_create_label_locked(
            state->screen, 10, UI_WORKFLOW_LINE_TOP + (int)(i * UI_WORKFLOW_LINE_HEIGHT) +
                (state->snapshot.step == WORKFLOW_STEP_RANGING && i > 0U ? UI_WORKFLOW_LINE_HEIGHT : 0),
            300, "", UI_COLOR_TEXT);
    }
    ui_workflow_create_remote_slots(state);
    if (state->snapshot.step == WORKFLOW_STEP_CALCULATING) {
        if (state->model.progress_indeterminate) {
            state->progress_spinner = lv_spinner_create(state->screen);
            if (state->progress_spinner != NULL) {
                lv_spinner_set_anim_params(state->progress_spinner, 1000U, 60U);
                lv_obj_set_size(state->progress_spinner, 40, 40);
                lv_obj_set_pos(state->progress_spinner, 140, 86);
            }
        } else {
            state->progress_bar = lv_bar_create(state->screen);
            if (state->progress_bar != NULL) {
                lv_obj_set_pos(state->progress_bar, 20, 68);
                lv_obj_set_size(state->progress_bar, 280, 12);
                lv_bar_set_range(state->progress_bar, 0, 100);
            }
        }
    }
    ui_workflow_create_action_slots(state);
}

static void ui_workflow_update_chart_labels(ui_workflow_screen_state_t *state)
{
    ui_context_t *context = ui_context();
    if (state->snapshot.step != WORKFLOW_STEP_RESULT ||
        state->snapshot.calculation_class != WORKFLOW_CALCULATION_CLASS_NONE) return;
    bool channel_set[SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT] = {false};
    for (size_t i = 0U; i < state->model.line_count; ++i) {
        const ui_workflow_model_line_t *line = &state->model.lines[i];
        if (line->key != UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE &&
            line->key != UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN) continue;
        const size_t channel = (size_t)strtoul(line->args[0], NULL, 10);
        if (channel == 0U || channel > SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT) continue;
        char text[48];
        ui_workflow_format_line(line, text, sizeof(text));
        ui_workflow_set_label(context->measurement_channel_labels[channel - 1U], text);
        channel_set[channel - 1U] = true;
    }
    for (size_t i = 0U; i < SEISMIC_RECORDER_WAVEFORM_CHANNEL_COUNT; ++i) {
        if (!channel_set[i]) {
            char channel[8], text[48];
            (void)snprintf(channel, sizeof(channel), "%u", (unsigned)(i + 1U));
            (void)ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN), channel);
            ui_workflow_set_label(context->measurement_channel_labels[i], text);
        }
    }
}

static void ui_workflow_update_actions(ui_workflow_screen_state_t *state)
{
    const size_t visible = ui_workflow_visible_actions(&state->snapshot, &state->model);
    size_t first = state->model.selected_action > visible / 2U
        ? state->model.selected_action - visible / 2U : 0U;
    if (first + visible > state->model.action_count) first = state->model.action_count - visible;
    const int start_y = 214 - (int)(visible * UI_WORKFLOW_ACTION_ROW_STEP);
    for (size_t slot = 0U; slot < UI_WORKFLOW_VISIBLE_ACTIONS; ++slot) {
        lv_obj_t *row = state->action_rows[slot];
        lv_obj_t *label = state->action_labels[slot];
        if (row == NULL || label == NULL) continue;
        if (slot >= visible) {
            if (!state->action_row_cache_valid[slot] || state->action_row_cache_visible[slot]) {
                lv_obj_set_hidden(row, true);
            }
            state->action_row_cache_valid[slot] = true;
            state->action_row_cache_visible[slot] = false;
            continue;
        }
        const size_t index = first + slot;
        const workflow_command_t command = state->model.actions[index];
        const bool selected = index == state->model.selected_action;
        char text[96];
        if (command == (workflow_command_t)WORKFLOW_COMMAND_COUNT) {
            ui_workflow_format_line(&state->model.material_action_line, text, sizeof(text));
        } else {
            (void)ui_utf8_format(text, sizeof(text), "%s", ui_tr(state->model.action_keys[index]));
        }
        if (state->model.pending && state->model.pending_command == command) {
            const size_t length = strlen(text);
            (void)ui_utf8_format(text + length, sizeof(text) - length, "%s", ui_tr(UI_I18N_KEY_TFT_WORKFLOW_VALUE_PENDING));
        }
        ui_workflow_set_label(label, text);
        const int row_y = start_y + (int)(slot * UI_WORKFLOW_ACTION_ROW_STEP);
        if (!state->action_row_cache_valid[slot] || state->action_row_cache_y[slot] != row_y) {
            lv_obj_set_pos(row, 10, row_y);
        }
        if (!state->action_row_cache_valid[slot] || state->action_row_cache_selected[slot] != selected) {
            lv_obj_set_style_bg_color(row, lv_color_hex(selected ? UI_COLOR_PANEL_ALT : UI_COLOR_PANEL), 0);
            lv_obj_set_style_border_color(row, lv_color_hex(selected ? UI_COLOR_ACCENT : UI_COLOR_PANEL_ALT), 0);
            lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
        }
        if (!state->action_row_cache_valid[slot] || !state->action_row_cache_visible[slot]) {
            lv_obj_set_hidden(row, false);
        }
        state->action_row_cache_valid[slot] = true;
        state->action_row_cache_visible[slot] = true;
        state->action_row_cache_selected[slot] = selected;
        state->action_row_cache_y[slot] = row_y;
    }
}

static void ui_workflow_update_lines(ui_workflow_screen_state_t *state)
{
    if (state->snapshot.step == WORKFLOW_STEP_RESULT &&
        state->snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_NONE) {
        for (size_t slot = 0U; slot < state->line_slots; ++slot) {
            char text[UI_WORKFLOW_MODEL_ARG_SIZE];
            const ui_workflow_model_line_t *line = slot < state->model.line_count
                ? &state->model.lines[slot] : NULL;
            if (line != NULL && line->key != UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE &&
                line->key != UI_I18N_KEY_TFT_WORKFLOW_RESULT_DISTANCE_UNKNOWN) {
                ui_workflow_format_line(line, text, sizeof(text));
            } else {
                text[0] = '\0';
            }
            ui_workflow_set_label(state->line_labels[slot], text);
        }
        ui_workflow_update_chart_labels(state);
    } else {
        for (size_t slot = 0U; slot < state->line_slots; ++slot) {
            char text[UI_WORKFLOW_MODEL_ARG_SIZE];
            if (slot < state->model.line_count) ui_workflow_format_line(&state->model.lines[slot], text, sizeof(text));
            else text[0] = '\0';
            ui_workflow_set_label(state->line_labels[slot], text);
        }
    }
    if (state->progress_bar != NULL && state->snapshot.step == WORKFLOW_STEP_RANGING) {
        if (state->ranging.available && state->ranging.pairs_total > 0U) {
            const size_t done = state->ranging.pairs_done < state->ranging.pairs_total
                ? state->ranging.pairs_done : state->ranging.pairs_total;
            lv_bar_set_range(state->progress_bar, 0, (int32_t)state->ranging.pairs_total);
            lv_bar_set_value(state->progress_bar, (int32_t)done, LV_ANIM_OFF);
            lv_obj_set_hidden(state->progress_bar, false);
        } else {
            lv_obj_set_hidden(state->progress_bar, true);
        }
    } else if (state->progress_bar != NULL && state->snapshot.progress >= 0) {
        lv_bar_set_value(state->progress_bar, state->snapshot.progress, LV_ANIM_OFF);
    }
    ui_workflow_update_actions(state);
    ui_workflow_update_remote_slots(state);
    ui_context_t *context = ui_context();
    const ui_i18n_key_t footer_key = state->model.notice_key != UI_I18N_KEY_COUNT
        ? state->model.notice_key : state->model.hint_key;
    if (context->footer_label != NULL && state->snapshot.step == WORKFLOW_STEP_SENSOR_CHECK &&
        state->model.notice_key == UI_I18N_KEY_COUNT) {
        char text[96];
        (void)ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_TFT_WORKFLOW_SENSOR_MAIN),
            ui_tr(state->preflight_adc), ui_tr(state->preflight_barometer),
            ui_tr(state->preflight_power), state->preflight_rail_v);
        ui_workflow_set_label(context->footer_label, text);
    } else if (context->footer_label != NULL && footer_key < UI_I18N_KEY_COUNT) {
        ui_workflow_set_label(context->footer_label, ui_tr(footer_key));
    }
    ui_workflow_apply_result_waveform(state);
}

static void ui_workflow_refresh_snapshot(ui_workflow_screen_state_t *state, uint64_t now_ms)
{
    workflow_snapshot_t *snapshot = &state->snapshot;
    const bool first = !state->snapshot_valid;
    const workflow_step_t old_step = snapshot->step;
    if (workflow_service_try_get_snapshot(snapshot) != ESP_OK) return;
    state->snapshot_valid = true;
    state->workflow_service_ready = true;

    if (snapshot->step == WORKFLOW_STEP_RECORDING && old_step != WORKFLOW_STEP_RECORDING) {
        const bool started_here = !first && old_step == WORKFLOW_STEP_RECORD_READY &&
            snapshot->last_async.command == WORKFLOW_COMMAND_START_RECORDING &&
            snapshot->last_async.result == WORKFLOW_COMMAND_RESULT_OK;
        state->model_state.recording_elapsed_valid = started_here;
        state->recording_started_ms = now_ms;
    }
    if (snapshot->step == WORKFLOW_STEP_RECORDING && state->model_state.recording_elapsed_valid) {
        state->model_state.recording_elapsed_ms = now_ms - state->recording_started_ms;
    }

    if (first || snapshot->step != old_step) {
        if (snapshot->step == WORKFLOW_STEP_RESULT &&
            snapshot->calculation_class == WORKFLOW_CALCULATION_CLASS_NONE) {
            state->model_state.result_data_ready = false;
            state->model_state.result_load_failed = false;
            state->model_state.result_waveform_available = false;
            state->model_state.result_source_valid = false;
            state->result_waveform_available = false;
            state->result_point_count = 0U;
            state->result_widget_data_applied = false;
            state->result_load_pending = false;
        }
        state->rendered_step = WORKFLOW_STEP_COUNT;
    }
    state->previous_step = snapshot->step;
}

static void ui_workflow_send_command(ui_workflow_screen_state_t *state, workflow_command_t command, uint64_t now_ms)
{
    if (!state->snapshot_valid || state->model_state.has_pending) return;
    uint32_t sequence = 0U;
    const esp_err_t err = workflow_service_command_async(
        command, true, state->snapshot.generation, &sequence);
    ui_workflow_model_command_sent(&state->model_state, command, err, sequence, now_ms);
}

static void ui_workflow_save_material(
    ui_workflow_screen_state_t *state,
    const ui_workflow_event_t *event,
    uint64_t now_ms)
{
    const size_t preset_count = localization_material_preset_count();
    const bool custom = event->material_index >= preset_count;
    const localization_material_preset_t *preset = custom
        ? NULL : localization_material_preset_at(event->material_index);
    if (!custom && preset == NULL) {
        ui_workflow_model_material_save_sent(
            &state->model_state, ESP_ERR_INVALID_ARG, 0U, now_ms);
        return;
    }

    uint32_t sequence = state->snapshot.last_async.seq;
    if (state->snapshot.last_material_save.seq > sequence) {
        sequence = state->snapshot.last_material_save.seq;
    }
    sequence++;
    if (sequence == 0U) sequence = 1U;
    const esp_err_t err = workflow_service_save_calc_material_range_async(
        custom ? "custom" : preset->id,
        custom ? event->custom_v_mps : 0U,
        custom ? event->custom_sigma_mps : 0U,
        custom && event->has_range,
        custom ? event->range_min_mps : 0U,
        custom ? event->range_max_mps : 0U);
    ui_workflow_model_material_save_sent(&state->model_state, err, sequence, now_ms);
}

static void ui_workflow_update(ui_workflow_screen_state_t *state, uint64_t now_ms)
{
    ui_workflow_refresh_remote_slots(state);
    ui_workflow_prepare_material_model(state);
    ui_workflow_model_build(&state->snapshot, &state->ranging, &state->model_state, now_ms, &state->model);
    if (state->screen == NULL || state->rendered_step != state->snapshot.step ||
        state->rendered_material_picker != state->model.material_picker_visible ||
        state->rendered_action_count != state->model.action_count ||
        state->line_slots != ui_workflow_line_slots(&state->snapshot, &state->model)) {
        ui_workflow_render_step_locked(state);
        if (state->snapshot.step == WORKFLOW_STEP_RESULT &&
            state->snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_NONE &&
            !state->model_state.result_data_ready && !state->result_load_pending) {
            if (!ui_workflow_queue_result_load(state)) {
                state->model_state.result_data_ready = true;
                state->model_state.result_load_failed = true;
                ui_workflow_model_build(&state->snapshot, &state->ranging, &state->model_state, now_ms, &state->model);
            }
        }
    }
    ui_workflow_update_lines(state);
}

void ui_workflow_screen_open_locked(void)
{
    if (s_workflow == NULL) {
        s_workflow = heap_caps_calloc(1U, sizeof(*s_workflow), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_workflow == NULL) {
            ui_screen_open_locked(UI_SCREEN_OBJECTS);
            return;
        }
        s_workflow->model_state.notice_key = UI_I18N_KEY_COUNT;
        s_workflow->rendered_step = WORKFLOW_STEP_COUNT;
        ui_context_t *context = ui_context();
        if (context->workflow_object_index < context->object_count) {
            (void)ui_utf8_format(s_workflow->model_state.active_object_name,
                sizeof(s_workflow->model_state.active_object_name), "%s",
                context->objects[context->workflow_object_index].name);
        }
    } else {
        s_workflow->screen = NULL;
        s_workflow->rendered_step = WORKFLOW_STEP_COUNT;
    }
    s_workflow->last_snapshot_ms = 0U;
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    ui_workflow_refresh_snapshot(s_workflow, now_ms);
    /* Opening the screen only renders the OBJECT-step action; the operator
     * explicitly sends START_MEASUREMENT by pressing OK. */
    s_workflow->last_snapshot_ms = now_ms;
    ui_workflow_build_ranging_view(s_workflow);
    ui_workflow_update(s_workflow, now_ms);
}

void ui_workflow_screen_close_locked(void)
{
    if (s_workflow != NULL) {
        heap_caps_free(s_workflow);
        s_workflow = NULL;
    }
}

void ui_workflow_screen_render_locked(void)
{
    if (s_workflow == NULL) return;
    s_workflow->screen = NULL;
    s_workflow->rendered_step = WORKFLOW_STEP_COUNT;
    s_workflow->rendered_material_picker = false;
    ui_workflow_update(s_workflow, (uint64_t)esp_timer_get_time() / 1000U);
}

void ui_workflow_screen_refresh_locked(void)
{
    if (s_workflow == NULL) return;
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    const bool refresh_due = s_workflow->last_snapshot_ms == 0U ||
        now_ms - s_workflow->last_snapshot_ms >= UI_WORKFLOW_REFRESH_PERIOD_MS;
    if (refresh_due) {
        ui_workflow_refresh_snapshot(s_workflow, now_ms);
        s_workflow->last_snapshot_ms = now_ms;
        ui_workflow_build_ranging_view(s_workflow);
        ui_workflow_update(s_workflow, now_ms);
    }
    if (s_workflow->snapshot.step == WORKFLOW_STEP_RECORDING) {
        seismic_acquisition_snapshot_t acquisition;
        const esp_err_t err = seismic_acquisition_get_snapshot(&acquisition);
        ui_measurements_refresh_charts_locked(err == ESP_OK ? &acquisition : NULL);
    }
}

void ui_workflow_screen_handle_key_locked(keypad_key_t key)
{
    if (s_workflow == NULL || !s_workflow->snapshot_valid) return;
    ui_workflow_key_t workflow_key;
    switch (key) {
        case KEYPAD_KEY_UP: workflow_key = UI_WORKFLOW_KEY_UP; break;
        case KEYPAD_KEY_DOWN: workflow_key = UI_WORKFLOW_KEY_DOWN; break;
        case KEYPAD_KEY_LEFT: workflow_key = UI_WORKFLOW_KEY_LEFT; break;
        case KEYPAD_KEY_RIGHT: workflow_key = UI_WORKFLOW_KEY_RIGHT; break;
        case KEYPAD_KEY_OK: workflow_key = UI_WORKFLOW_KEY_OK; break;
        default: return;
    }
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    ui_workflow_prepare_material_model(s_workflow);
    const ui_workflow_event_t event = ui_workflow_model_handle_key(
        &s_workflow->snapshot, &s_workflow->model_state, workflow_key, now_ms);
    switch (event.type) {
        case UI_WORKFLOW_EVENT_SEND_COMMAND:
            ui_workflow_send_command(s_workflow, event.command, now_ms);
            break;
        case UI_WORKFLOW_EVENT_LEAVE_OBJECTS:
            ui_screen_open_locked(UI_SCREEN_OBJECTS);
            return;
        case UI_WORKFLOW_EVENT_LEAVE_HOME:
            ui_screen_open_locked(UI_SCREEN_HOME);
            return;
        case UI_WORKFLOW_EVENT_OPEN_PREVIEW:
            ui_scan_preview_open_from_workflow_locked(
                s_workflow->snapshot.object_id, s_workflow->snapshot.session_id);
            return;
        case UI_WORKFLOW_EVENT_SAVE_MATERIAL:
            ui_workflow_save_material(s_workflow, &event, now_ms);
            break;
        case UI_WORKFLOW_EVENT_NONE:
        case UI_WORKFLOW_EVENT_DELETE_CONFIRM_CHANGED:
        default:
            break;
    }
    ui_workflow_update(s_workflow, now_ms);
}
