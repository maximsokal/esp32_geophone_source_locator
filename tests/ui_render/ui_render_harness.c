#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/libs/lodepng/lodepng.h"
#include "ui_fonts.h"
#ifdef UI_NATIVE_FULL_SCREENS
#include "ui_navigation_fixture.h"
#endif

/* Include the complete production screen, so assertions can inspect its widgets.
 * No extracted/reimplemented builders, LVGL stubs or test-only production hooks. */
#ifdef _MSC_VER
/* The SDK-free critical section in the asynchronous file loader confuses
 * MSVC's definite-assignment analysis; production guards request with have_request. */
#pragma warning(push)
#pragma warning(disable: 4701)
#endif
#include "../../components/ui/ui_workflow.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

extern ui_context_t render_context;
extern workflow_snapshot_t render_snapshot;
extern device_status_snapshot_t render_device_status;
extern bool render_ranging_available;
extern nrf_topology_ranging_progress_t render_ranging_progress;
extern ranging_radio_state_t render_radio_state;
extern workflow_command_params_t render_distance_params;
extern unsigned render_distance_calls;
extern nrf_node_link_status_input_t render_node_status[UI_WORKFLOW_REMOTE_SLOT_COUNT + 1U];
extern nrf_topology_mode_t render_topology_mode;
extern uint64_t render_now_ms;
extern workflow_command_t render_last_command;
extern unsigned render_command_count;
extern esp_err_t render_workflow_snapshot_result;
extern esp_err_t render_device_status_result;
extern unsigned render_delete_calls;
extern measurement_service_snapshot_t render_measurement;

static FILE *frames;
static const char *output_dir;
static unsigned frame_count;
static const char *menu_family = "home";
void render_all_menus(void);
const char *render_menus_widget_role(lv_obj_t *object);
int render_preview_status(void);

static void json_text(const char *text)
{
    fputc('"', frames);
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', frames);
        if (*p < 32U) fprintf(frames, "\\u%04x", (unsigned)*p);
        else fputc(*p, frames);
    }
    fputc('"', frames);
}

static void widget_role(lv_obj_t *object, char *role, size_t size)
{
    role[0] = '\0';
    if (object == render_context.root) (void)snprintf(role, size, "screen");
    else if (object == render_context.header_source) (void)snprintf(role, size, "header_source");
    else if (object == render_context.header_wifi) (void)snprintf(role, size, "header_wifi");
    else if (object == render_context.header_power) (void)snprintf(role, size, "header_power");
    else if (object == render_context.footer_label) (void)snprintf(role, size, "footer");
    else if (object == render_context.measurement_graphs) (void)snprintf(role, size, "graphs");
    else if (object == render_context.measurement_content) (void)snprintf(role, size, "measurement_content");
    else if (object == render_context.measurement_health) (void)snprintf(role, size, "measurement_health");
    for (size_t i = 0U; i < UI_MEASUREMENTS_NODE_COUNT; ++i) {
        if (object == render_context.measurement_node_labels[i]) (void)snprintf(role, size, "measurement_node_%u", (unsigned)i);
    }
    for (size_t i = 0U; i < UI_HOME_SCREEN_COUNT; ++i) {
        if (object == render_context.home_rows[i]) (void)snprintf(role, size, "home_%u", (unsigned)i);
    }
    if (role[0] == '\0') (void)snprintf(role, size, "%s", render_menus_widget_role(object));
    if (s_workflow == NULL) return;
    if (object == s_workflow->progress_bar || object == s_workflow->progress_spinner)
        (void)snprintf(role, size, "progress");
    for (size_t i = 0U; i < UI_WORKFLOW_VISIBLE_ACTIONS; ++i) {
        if (object == s_workflow->action_rows[i]) (void)snprintf(role, size, "action_%u", (unsigned)i);
        if (object == s_workflow->action_labels[i]) (void)snprintf(role, size, "action_text_%u", (unsigned)i);
    }
    for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
        if (object == s_workflow->remote_slot_cards[i]) (void)snprintf(role, size, "remote_%u", (unsigned)i);
        if (object == s_workflow->remote_slot_labels[i]) (void)snprintf(role, size, "remote_text_%u", (unsigned)i);
    }
    for (size_t i = 0U; i < UI_WORKFLOW_MODEL_MAX_LINES; ++i) {
        if (object == s_workflow->line_labels[i]) (void)snprintf(role, size, "line_%u", (unsigned)i);
    }
}

static void dump_widget(lv_obj_t *object, int parent, bool parent_hidden, unsigned *next_id)
{
    const unsigned id = (*next_id)++;
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const bool hidden = parent_hidden || lv_obj_is_hidden(object);
    char role[32];
    widget_role(object, role, sizeof(role));
    if (id != 0U) fputc(',', frames);
    fprintf(frames, "{\"id\":%u,\"parent\":%d,\"role\":", id, parent);
    json_text(role);
    fprintf(frames, ",\"bounds\":[%ld,%ld,%ld,%ld],\"hidden\":%s,\"border\":%ld,\"bg\":%lu,\"border_color\":%lu",
        (long)area.x1, (long)area.y1, (long)area.x2, (long)area.y2, hidden ? "true" : "false",
        (long)lv_obj_get_style_border_width(object, 0),
        (unsigned long)lv_color_to_u32(lv_obj_get_style_bg_color(object, 0)),
        (unsigned long)lv_color_to_u32(lv_obj_get_style_border_color(object, 0)));
    fprintf(frames, ",\"kind\":\"%s\"", lv_obj_check_type(object, &lv_label_class) ? "label" :
        lv_obj_check_type(object, &lv_chart_class) ? "chart" :
        lv_obj_check_type(object, &lv_canvas_class) ? "canvas" : "object");
    fprintf(frames, ",\"scroll_viewport\":%s,\"clips_children\":%s,\"scroll_y\":%ld,\"scroll_bottom\":%ld",
        lv_obj_is_scrollable(object) && lv_obj_get_scroll_dir(object) == LV_DIR_VER &&
            !lv_obj_is_overflow_visible(object) ? "true" : "false",
        !lv_obj_is_overflow_visible(object) ? "true" : "false",
        (long)lv_obj_get_scroll_y(object), (long)lv_obj_get_scroll_bottom(object));
    if (lv_obj_check_type(object, &lv_label_class)) {
        const char *text = lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
        lv_point_t natural_size;
        lv_text_get_size(&natural_size, text, font, lv_obj_get_style_text_letter_space(object, 0),
            lv_obj_get_style_text_line_space(object, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        fprintf(frames, ",\"text\":");
        json_text(text);
        fprintf(frames, ",\"natural\":[%ld,%ld],\"font_body\":%s,\"color\":%lu,\"clips_text\":%s",
            (long)natural_size.x, (long)natural_size.y, font == &ui_font_montserrat_14 ? "true" : "false",
            (unsigned long)lv_color_to_u32(lv_obj_get_style_text_color(object, 0)),
            lv_label_get_long_mode(object) == LV_LABEL_LONG_CLIP ? "true" : "false");
    }
    fputc('}', frames);
    for (uint32_t child = 0U; child < lv_obj_get_child_count(object); ++child) {
        dump_widget(lv_obj_get_child(object, (int32_t)child), (int)id, hidden, next_id);
    }
}

static void save_png(const char *name)
{
    lv_draw_buf_t *buffer = lv_display_get_buf_active(render_context.display);
    unsigned char *rgba = malloc(320U * 240U * 4U);
    assert(rgba != NULL && buffer->header.cf == LV_COLOR_FORMAT_RGB565);
    for (size_t y = 0U; y < 240U; ++y) {
        const uint16_t *row = (const uint16_t *)(buffer->data + y * buffer->header.stride);
        for (size_t x = 0U; x < 320U; ++x) {
            const uint16_t pixel = row[x];
            unsigned char *out = rgba + (y * 320U + x) * 4U;
            /* Same RGB565 rounding as LVGL's screenshot comparator. */
            out[0] = (unsigned char)(((pixel >> 11U) * 2106U) >> 8U);
            out[1] = (unsigned char)((((pixel >> 5U) & 63U) * 1037U) >> 8U);
            out[2] = (unsigned char)(((pixel & 31U) * 2106U) >> 8U);
            out[3] = 255U;
        }
    }
    char path[512];
    (void)snprintf(path, sizeof(path), "S:%s/%s.png", output_dir, name);
    assert(lodepng_encode32_file(path, rgba, 320U, 240U) == 0U);
    free(rgba);
}

static void capture(const char *name, bool golden)
{
    lv_obj_update_layout(render_context.root);
    lv_refr_now(render_context.display);
    save_png(name);
    char reference[512];
    (void)snprintf(reference, sizeof(reference), "S:%s/references/%s.png", output_dir, name);
    const int comparison = golden ? (int)lv_test_screenshot_compare(reference) : -1;
    fprintf(frames, "{\"name\":");
    json_text(name);
    fputs(",\"family\":", frames);
#ifdef UI_NATIVE_FULL_SCREENS
    json_text(menu_family);
#else
    json_text(s_workflow != NULL ? "workflow" : menu_family);
#endif
    fprintf(frames, ",\"screen\":%d", (int)render_context.screen);
    fprintf(frames, ",\"objects_mode\":%d,\"delete_calls\":%u", (int)render_context.objects_mode, render_delete_calls);
    fprintf(frames, ",\"wifi_confirm\":%d,\"measurement_state\":%d,\"preview_status\":%d",
        (int)render_context.wifi_confirm, (int)render_measurement.state, render_preview_status());
    fprintf(frames, ",\"golden\":%s,\"comparison\":%d,\"step\":%d,\"remotes\":%u,\"actions\":%u,\"selected\":%u,\"widgets\":[",
        golden ? "true" : "false", comparison, s_workflow != NULL ? (int)s_workflow->snapshot.step : -1,
        s_workflow != NULL ? (unsigned)s_workflow->snapshot.assigned_remotes : 0U,
        s_workflow != NULL ? (unsigned)s_workflow->model.action_count : 0U,
        s_workflow != NULL ? (unsigned)s_workflow->model.selected_action : 0U);
    unsigned next_id = 0U;
    dump_widget(render_context.root, -1, false, &next_id);
    fputs("]}\n", frames);
    ++frame_count;
}

void render_capture_menu(const char *family, const char *variant)
{
    menu_family = family;
    char name[112];
    (void)snprintf(name, sizeof(name), "%s_menu_%s_%s",
        ui_i18n_get_locale() == UI_LOCALE_EN ? "en" : "uk", family, variant);
    capture(name, true);
}

#ifdef UI_NATIVE_FULL_SCREENS
void render_navigation_start(const char *directory)
{
    output_dir = directory;
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/frames.jsonl", directory);
    frames = fopen(path, "wb");
    assert(frames != NULL);
    lv_init();
    render_context.display = lv_test_display_create(320, 240);
    lv_display_set_color_format(render_context.display, LV_COLOR_FORMAT_RGB565);
}

void render_navigation_capture(const char *name)
{
    static const char *families[] = {"home", "objects", "measurements", "storage", "wifi", "system", "preview", "workflow"};
    menu_family = families[render_context.screen];
    capture(name, false);
}

void render_navigation_finish(void) { fclose(frames); lv_deinit(); }
bool render_workflow_exists(void) { return s_workflow != NULL; }
bool render_workflow_result_pending(void) { return s_workflow != NULL && s_workflow->result_load_pending; }
bool render_workflow_result_ready(void) { return s_workflow != NULL && s_workflow->model_state.result_data_ready; }
uint32_t render_workflow_result_generation(void) { return s_workflow != NULL ? s_workflow->result_generation : 0U; }
size_t render_workflow_action_count(void) { return s_workflow != NULL ? s_workflow->model.action_count : 0U; }
#else
static void prepare(workflow_step_t step, unsigned remotes)
{
    ui_workflow_screen_close_locked();
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = step;
    core.calibration_ok = true;
    core.ranging_ok = true;
    core.calc_settings_ready = true;
    workflow_core_snapshot(&core, remotes, true, &render_snapshot);
    (void)snprintf(render_snapshot.object_id, sizeof(render_snapshot.object_id), "fixture");
    (void)snprintf(render_snapshot.calibration.phase, sizeof(render_snapshot.calibration.phase), "complete");
    render_snapshot.calibration.result_valid = step == WORKFLOW_STEP_CALIB_RESULT;
    render_snapshot.calibration.result_ok = true;
    render_snapshot.calibration.per_node_count = remotes + 1U;
    /* Calibration is height + IMU only; Channel Sounding runs at ranging. */
    render_snapshot.calibration.pairs_done = 0U;
    render_snapshot.calibration.pairs_total = 0U;
    render_snapshot.placement.per_node_count = remotes + 1U;
    render_snapshot.placement.all_stable = true;
    for (size_t i = 0U; i < remotes + 1U; ++i) {
        workflow_calibration_node_t *node = &render_snapshot.calibration.per_node[i];
        (void)snprintf(node->node, sizeof(node->node), i == 0U ? "MAIN" : "G%u", (unsigned)i);
        (void)snprintf(node->height_state, sizeof(node->height_state), "ready");
        /* MAIN has no IMU; the service reports it as not_applicable. */
        (void)snprintf(node->imu_state, sizeof(node->imu_state), "%s", i == 0U ? "not_applicable" : "ready");
        node->relative_z_mm_valid = true;
        node->relative_z_mm = (int32_t)i * 12;
        workflow_placement_node_t *placed = &render_snapshot.placement.per_node[i];
        (void)snprintf(placed->node, sizeof(placed->node), "%s", node->node);
        placed->vertical = true;
        placed->stable = true;
        placed->height_rel_main_mm_valid = true;
        placed->height_rel_main_mm = (int64_t)i * 12;
    }
    memset(&render_device_status, 0, sizeof(render_device_status));
    memset(render_node_status, 0, sizeof(render_node_status));
    for (size_t i = 0U; i <= remotes; ++i) {
        render_node_status[i] = (nrf_node_link_status_input_t){
            .assigned = true, .status_valid = true, .status_identity_matches = true,
            .lifecycle = NRF_UPSTREAM_LIFECYCLE_READY, .result = NRF_UPSTREAM_RESULT_OK,
            .flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED,
            .status_receipt_uptime_ms = render_now_ms,
        };
    }
    for (size_t i = 0U; i < 4U; ++i) render_device_status.pods[i].state =
        i < remotes ? POD_STATE_READY : POD_STATE_DISCONNECTED;
    render_context.object_count = 1U;
    render_context.workflow_object_index = 0U;
    (void)snprintf(render_context.objects[0].name, sizeof(render_context.objects[0].name), "Bridge A");
    render_context.screen = UI_SCREEN_WORKFLOW;
    ui_workflow_screen_open_locked();
    assert(s_workflow != NULL);
}

static void inject_result(void)
{
    s_workflow->model_state.result_data_ready = true;
    s_workflow->model_state.result_load_failed = false;
    s_workflow->model_state.result_waveform_available = true;
    s_workflow->model_state.result_source_valid = true;
    s_workflow->model_state.result_source_m[0] = 1.25;
    s_workflow->model_state.result_source_m[1] = 2.5;
    s_workflow->model_state.result_source_m[2] = -0.3;
    s_workflow->result_waveform_available = true;
    s_workflow->result_point_count = UI_WORKFLOW_RESULT_POINTS;
    for (size_t c = 0U; c < 4U; ++c) {
        for (size_t p = 0U; p < UI_WORKFLOW_RESULT_POINTS; ++p) {
            const float wave = (float)((int)((p + c * 7U) % 19U) - 9) * 120.0f;
            s_workflow->result_points[p].min_uv[c] = wave - 70.0f;
            s_workflow->result_points[p].max_uv[c] = wave + 70.0f;
        }
        s_workflow->result_arrival_valid[c] = true;
        s_workflow->result_arrival_point[c] = 12U + c * 6U;
    }
    s_workflow->result_widget_data_applied = false;
    ui_workflow_update(s_workflow, render_now_ms);
}

static unsigned s_remote_invalidations;

static void count_remote_invalidations(lv_event_t *event)
{
    (void)event;
    ++s_remote_invalidations;
}

static void test_unchanged_sensor_cards_do_not_redraw(void)
{
    prepare(WORKFLOW_STEP_SENSOR_CHECK, 1U);
    lv_display_t *display = lv_display_get_default();
    lv_refr_now(display);
    lv_event_dsc_t *listener = lv_display_add_event_cb(
        display, count_remote_invalidations, LV_EVENT_INVALIDATE_AREA, NULL);
    assert(listener != NULL);
    s_remote_invalidations = 0U;
    s_workflow->remote_slot_styles_dirty = true;
    ui_workflow_update_remote_slots(s_workflow);
    assert(s_remote_invalidations == 0U);
    s_workflow->remote_pod_states[0] = POD_STATE_ERROR;
    s_workflow->remote_slot_styles_dirty = true;
    ui_workflow_update_remote_slots(s_workflow);
    assert(s_remote_invalidations > 0U);
    assert(lv_display_remove_event_cb_with_user_data(display, count_remote_invalidations, NULL) == 1U);
}

static void test_ranging_manual_screens(const char *language, workflow_core_t *core)
{
    for (size_t i = 0U; s_workflow->model_state.selected_action != WORKFLOW_COMMAND_SET_DISTANCE; ++i) {
        assert(i < s_workflow->model.action_count);
        ui_workflow_screen_handle_key_locked(KEYPAD_KEY_DOWN);
    }
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
    assert(s_workflow->model_state.distance_view == UI_WORKFLOW_DISTANCE_PAIR);
    assert(s_workflow->model_state.distance_pair == 3U);
    char name[96];
    (void)snprintf(name, sizeof(name), "%s_ranging_manual_pair", language);
    capture(name, true);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
    assert(s_workflow->model_state.distance_view == UI_WORKFLOW_DISTANCE_VALUE);
    (void)snprintf(name, sizeof(name), "%s_ranging_manual_value", language);
    capture(name, true);
    const unsigned calls = render_distance_calls;
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
    assert(render_distance_calls == calls + 1U && render_distance_params.distance_mm == 6001);
    render_snapshot.last_async = (workflow_last_async_t){
        .seq = s_workflow->model_state.pending_seq, .command = WORKFLOW_COMMAND_SET_DISTANCE,
        .result = WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT,
    };
    s_workflow->last_snapshot_ms = 0U;
    ui_workflow_screen_refresh_locked();
    assert(s_workflow->model_state.distance_view == UI_WORKFLOW_DISTANCE_VALUE);
    (void)snprintf(name, sizeof(name), "%s_ranging_manual_rejected", language);
    capture(name, true);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
    for (unsigned i = 0U; i < 5U; ++i) ui_workflow_screen_handle_key_locked(KEYPAD_KEY_DOWN);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
    for (unsigned i = 0U; i < 5U; ++i) ui_workflow_screen_handle_key_locked(KEYPAD_KEY_UP);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_DOWN);
    assert(s_workflow->model_state.distance_mm == 1500U);
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
    assert(render_distance_calls == calls + 2U);
    assert(render_distance_params.node_a == 0U && render_distance_params.node_b == 4U);
    assert(render_distance_params.distance_mm == 1500);
    (void)snprintf(name, sizeof(name), "%s_ranging_manual_saving", language);
    capture(name, true);
    const uint32_t seq = s_workflow->model_state.pending_seq;
    workflow_command_params_t accepted = render_distance_params;
    accepted.has_ok = true; accepted.ok = true;
    assert(workflow_core_apply(core, WORKFLOW_COMMAND_SET_DISTANCE, &accepted, 4U, true) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_core_snapshot(core, 4U, true, &render_snapshot);
    render_snapshot.last_async = (workflow_last_async_t){
        .seq = seq, .command = WORKFLOW_COMMAND_SET_DISTANCE, .result = WORKFLOW_COMMAND_RESULT_OK,
    };
    render_ranging_progress.pairs[3].state = NRF_CS_PAIR_VALID;
    render_ranging_progress.pairs[3].distance_mm = 1500;
    render_ranging_progress.pairs[3].automatic_distance_mm = 6001;
    render_ranging_progress.pairs[3].scheduler_quality_flags |= NRF_CS_QUALITY_MANUAL;
    render_ranging_progress.success = true;
    render_ranging_progress.partial = false;
    ++render_ranging_progress.updated_at_ms;
    s_workflow->last_snapshot_ms = 0U;
    ui_workflow_screen_refresh_locked();
    assert(s_workflow->model_state.distance_view == UI_WORKFLOW_DISTANCE_CLOSED);
    assert((s_workflow->snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM)) != 0U);
    size_t page = 0U;
    do {
        (void)snprintf(name, sizeof(name), "%s_ranging_manual_saved_page%u", language, (unsigned)++page);
        capture(name, true);
        ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
        assert(page <= UI_WORKFLOW_MODEL_MAX_LINES);
    } while (s_workflow->ranging_line_offset != 0U);
}

/*
 * The plan MAIN runs holds one execution per ordered pair of the connected nodes, so its
 * "pairs_total" is nodes * (nodes - 1) and the same number of finished windows is a smaller
 * share of a bigger network.
 */
static void test_ranging_progress_scene(const char *language, const char *suffix,
    unsigned nodes, unsigned done)
{
    char name[96];
    prepare(WORKFLOW_STEP_RANGING, nodes - 1U);
    s_workflow->ranging.available = true;
    s_workflow->ranging.pairs_done = done;
    s_workflow->ranging.pairs_total = nodes * (nodes - 1U);
    s_workflow->ranging.residual_valid = true;
    s_workflow->ranging.residual_mm = 12;
    s_workflow->ranging.node_count = nodes;
    for (size_t node = 0U; node < nodes; ++node) {
        (void)snprintf(s_workflow->ranging.nodes[node], sizeof(s_workflow->ranging.nodes[node]),
            node == 0U ? "MAIN" : "G%u", (unsigned)node);
        s_workflow->ranging.sigma_mm[node] = 10;
    }
    ui_workflow_update(s_workflow, render_now_ms);
    (void)snprintf(name, sizeof(name), "%s_ranging_progress%s", language, suffix);
    capture(name, true);
}

/*
 * The Wi-Fi radio goes off while the nodes range and comes back by itself: the page says so before it goes off (the nodes wait for
 * it), while it is off, and on the result page while the radio returns.
 */
static bool wifi_line_shown(void)
{
    for (size_t slot = 0U; slot < s_workflow->line_slots; ++slot) {
        if (strstr(lv_label_get_text(s_workflow->line_labels[slot]), "Wi-Fi") != NULL) return true;
    }
    return false;
}

static void test_ranging_wifi_screens(const char *language)
{
    char name[96];

    test_ranging_progress_scene(language, "_without_wifi_line", 5U, 3U);
    assert(!wifi_line_shown());                                       /* the radio is on: the page says nothing about it */

    prepare(WORKFLOW_STEP_RANGING, 4U);
    s_workflow->ranging.radio_state = UI_WORKFLOW_RADIO_PENDING;      /* announced, the nodes have not started */
    ui_workflow_update(s_workflow, render_now_ms);
    assert(wifi_line_shown());
    (void)snprintf(name, sizeof(name), "%s_ranging_wifi_pending", language);
    capture(name, true);

    prepare(WORKFLOW_STEP_RANGING, 4U);
    s_workflow->ranging.available = true;
    s_workflow->ranging.radio_state = UI_WORKFLOW_RADIO_OFF;
    s_workflow->ranging.pairs_done = 3U;
    s_workflow->ranging.pairs_total = 20U;
    s_workflow->ranging.node_count = 5U;
    ui_workflow_update(s_workflow, render_now_ms);
    assert(wifi_line_shown());
    (void)snprintf(name, sizeof(name), "%s_ranging_wifi_off", language);
    capture(name, true);

    prepare(WORKFLOW_STEP_RANGING_RESULT, 4U);
    s_workflow->ranging.radio_state = UI_WORKFLOW_RADIO_RESUMING;
    ui_workflow_update(s_workflow, render_now_ms);
    assert(wifi_line_shown());
    (void)snprintf(name, sizeof(name), "%s_ranging_wifi_back", language);
    capture(name, true);
}

/*
 * 2026-10-31 bench: after a calibration run OK on the result screen walked BACK instead of to the placement
 * step. BACK is the only action while the run is in progress; the highlight stayed on it into the next step.
 */
/*
 * The placement step while a node is not stable: "Sensors placed" is on the list but disabled, the screen
 * names the node, the reason and the numbers, and the live geophone view is reachable from here.
 */
static void test_placement_blocked_screens(const char *language)
{
    static const struct {
        const char *name;
        workflow_placement_reason_t reason;
        uint8_t samples;
        int32_t range_mm;
        bool vertical;
    } cases[] = {
        {"range", WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED, 9U, 134, true},
        {"tilted", WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL, 0U, -1, false},
        {"collecting", WORKFLOW_PLACEMENT_REASON_COLLECTING, 2U, -1, true},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        prepare(WORKFLOW_STEP_PLACEMENT, 4U);
        render_snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_BACK);
        render_snapshot.placement.all_stable = false;
        workflow_placement_node_t *node = &render_snapshot.placement.per_node[2];
        node->stable = false;
        node->vertical = cases[index].vertical;
        node->reason = cases[index].reason;
        node->sample_count = cases[index].samples;
        node->range_mm = cases[index].range_mm;
        s_workflow->last_snapshot_ms = 0U;
        ui_workflow_screen_refresh_locked();

        assert(s_workflow->snapshot.step == WORKFLOW_STEP_PLACEMENT);
        assert(s_workflow->model.action_count == 2U);
        assert(s_workflow->model.actions[0] == UI_WORKFLOW_ACTION_PLACEMENT_BLOCKED);
        assert(s_workflow->model.actions[1] == WORKFLOW_COMMAND_BACK);
        assert(s_workflow->model.selected_action == 0U);
        char name[96];
        (void)snprintf(name, sizeof(name), "%s_placement_blocked_%s", language, cases[index].name);
        capture(name, true);

        if (index == 0U) {
            /* OK on the disabled button sends nothing and explains instead. */
            const unsigned before = render_command_count;
            ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
            assert(render_command_count == before);
            assert(s_workflow->model_state.notice_key == UI_I18N_KEY_TFT_WORKFLOW_NOTICE_PLACEMENT_BLOCKED);
            (void)snprintf(name, sizeof(name), "%s_placement_blocked_notice", language);
            capture(name, true);
        }
    }
}

static void test_calibration_result_selection_after_run(const char *language)
{
    static workflow_snapshot_t result_snapshot;
    prepare(WORKFLOW_STEP_CALIB_RESULT, 4U);
    result_snapshot = render_snapshot;

    prepare(WORKFLOW_STEP_CALIBRATING, 4U);
    assert(s_workflow->model.action_count == 1U);
    assert(s_workflow->model.actions[0] == WORKFLOW_COMMAND_BACK);

    render_snapshot = result_snapshot;
    s_workflow->last_snapshot_ms = 0U;
    ui_workflow_screen_refresh_locked();
    assert(s_workflow->snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(s_workflow->model.action_count >= 2U);
    assert(s_workflow->model.actions[s_workflow->model.selected_action] == WORKFLOW_COMMAND_PLACE_SENSORS);

    char name[96];
    (void)snprintf(name, sizeof(name), "%s_calibration_result_after_run", language);
    capture(name, true);

    const unsigned before = render_command_count;
    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
    assert(render_command_count == before + 1U && render_last_command == WORKFLOW_COMMAND_PLACE_SENSORS);
}

static void test_ranging_result_screens(const char *language)
{
    static const char *outcomes[] = {"success", "partial", "failed", "cable_exceeded", "pair_diagnostics", "partial_usable"};
    static const char *reasons[] = {NULL, "pair_missing:0-4", "no_measurements", "cable_exceeded:0-4", "pair_failed:0-4", NULL};
    for (size_t outcome = 0U; outcome < sizeof(outcomes) / sizeof(outcomes[0]); ++outcome) {
        render_ranging_available = false;
        prepare(WORKFLOW_STEP_RANGING, 4U);
        memset(&render_ranging_progress, 0, sizeof(render_ranging_progress));
        render_ranging_available = true;
        render_ranging_progress.partial = outcome == 1U || outcome == 3U || outcome == 4U || outcome == 5U;
        render_ranging_progress.complete = true;
        render_ranging_progress.pair_count = UI_WORKFLOW_RANGING_MAX_PAIRS;
        render_ranging_progress.pairs_done = UI_WORKFLOW_RANGING_MAX_PAIRS;
        render_ranging_progress.pairs_total = UI_WORKFLOW_RANGING_MAX_PAIRS;
        render_ranging_progress.solution_residual_mm = outcome == 2U ? INT32_MAX : 12;
        size_t index = 0U;
        for (uint8_t a = 0U; a < 4U; ++a) {
            for (uint8_t b = a + 1U; b <= 4U; ++b) {
                nrf_cs_pair_result_t *pair = &render_ranging_progress.pairs[index];
                pair->node_a = a;
                pair->node_b = b;
                pair->state = outcome == 2U ? NRF_CS_PAIR_MISSING : NRF_CS_PAIR_VALID;
                pair->distance_mm = outcome == 2U ? 0 : (int32_t)(1234U + index);
                if (index == 3U && (outcome == 1U || outcome == 5U)) {
                    pair->state = NRF_CS_PAIR_MISSING;
                    pair->distance_mm = 0;
                } else if (index == 3U && outcome == 3U) {
                    pair->state = NRF_CS_PAIR_CABLE_EXCEEDED;
                    pair->distance_mm = 6001;
                } else if (index == 3U && outcome == 4U) {
                    /* Bench 2026-11-03: MAIN->G4 8.8 m (3 results, 1 above the limit), G4->MAIN 6.4 m (10 results, 9 accepted). */
                    pair->state = NRF_CS_PAIR_RETRY_EXHAUSTED;
                    pair->distance_mm = 0;
                    pair->diag_distance_mm[0] = 8845; pair->diag_distance_mm[1] = 6421;
                    pair->diag_samples[0] = 3U; pair->diag_samples[1] = 10U;
                    pair->diag_accepted[0] = 1U; pair->diag_accepted[1] = 9U;
                }
                ++index;
            }
        }
        assert(index == UI_WORKFLOW_RANGING_MAX_PAIRS);

        workflow_core_t core;
        workflow_core_init(&core, true);
        core.step = WORKFLOW_STEP_RANGING;
        const workflow_command_params_t params = {
            .has_ok = true, .ok = outcome == 0U || outcome == 5U, .reason = reasons[outcome],
        };
        assert(workflow_core_apply(&core, WORKFLOW_COMMAND_RANGING_DONE, &params, 4U, true) ==
            WORKFLOW_COMMAND_RESULT_OK);
        workflow_core_snapshot(&core, 4U, true, &render_snapshot);
        (void)snprintf(render_snapshot.object_id, sizeof(render_snapshot.object_id), "fixture");
        s_workflow->last_snapshot_ms = 0U;
        ui_workflow_screen_refresh_locked();
        assert(s_workflow->snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
        assert(s_workflow->model.lines[0].key == (outcome == 0U
            ? UI_I18N_KEY_TFT_WORKFLOW_RANGING_SUCCESS : outcome == 2U
            ? UI_I18N_KEY_TFT_WORKFLOW_RANGING_FAILED : UI_I18N_KEY_TFT_WORKFLOW_RANGING_PARTIAL));
        /* A result screen opens on its first (forward) action, not on the BACK left over from RANGING. */
        assert(s_workflow->model.actions[s_workflow->model.selected_action] != WORKFLOW_COMMAND_BACK);

        unsigned pair_mask = 0U;
        bool both_directions_shown = false, cause_shown = false;
        size_t page = 0U;
        do {
            char name[96];
            (void)snprintf(name, sizeof(name), "%s_ranging_result_%s_page%u",
                language, outcomes[outcome], (unsigned)++page);
            capture(name, true);
            for (size_t slot = 0U; slot < s_workflow->line_slots; ++slot) {
                const char *text = lv_label_get_text(s_workflow->line_labels[slot]);
                if (strstr(text, "885") != NULL && strstr(text, "642") != NULL) both_directions_shown = true;
                if (strstr(text, ui_tr(UI_I18N_KEY_TFT_WORKFLOW_RANGING_CAUSE_LOW_CONFIDENCE)) != NULL) cause_shown = true;
                for (size_t i = 0U; i < UI_WORKFLOW_RANGING_MAX_PAIRS; ++i) {
                    char expected[16];
                    (void)snprintf(expected, sizeof(expected), "%u-%u:",
                        (unsigned)render_ranging_progress.pairs[i].node_a,
                        (unsigned)render_ranging_progress.pairs[i].node_b);
                    if (strstr(text, expected) != NULL) pair_mask |= 1U << i;
                }
            }
            ui_workflow_screen_handle_key_locked(KEYPAD_KEY_RIGHT);
            assert(page <= UI_WORKFLOW_MODEL_MAX_LINES);
        } while (s_workflow->ranging_line_offset != 0U);
        assert(page > 1U);
        assert(pair_mask == (1U << UI_WORKFLOW_RANGING_MAX_PAIRS) - 1U);
        /* The pair without a distance shows what each direction measured and why it was not used. */
        assert((outcome == 4U) == both_directions_shown && (outcome == 4U) == cause_shown);
        if (outcome == 3U) test_ranging_manual_screens(language, &core);
    }
    render_ranging_available = false;
}

/*
 * Critical battery: the device is going to stop. The red bar over the footer must be on every
 * screen the user can be on while a START, RANGING or recording is running, and nowhere else.
 */
static void set_battery_phase(device_low_battery_phase_t phase, uint32_t seconds)
{
    render_device_status.power.valid = true;
    render_device_status.power.state = phase == DEVICE_LOW_BATTERY_NORMAL
        ? DEVICE_POWER_STATE_BATTERY : DEVICE_POWER_STATE_BATTERY_CRITICAL;
    render_device_status.power.low_battery = phase;
    render_device_status.power.shutdown_in_s = seconds;
    ui_refresh_header_locked();
}

static void test_critical_battery_screens(const char *language)
{
    static const struct { workflow_step_t step; const char *scene; } workflow_scenes[] = {
        {WORKFLOW_STEP_RANGING, "ranging"},
        {WORKFLOW_STEP_CALCULATING, "calculating"},
        {WORKFLOW_STEP_RECORDING, "recording"},
    };
    char name[96];
    for (size_t i = 0U; i < sizeof(workflow_scenes) / sizeof(workflow_scenes[0]); ++i) {
        prepare(workflow_scenes[i].step, 4U);
        set_battery_phase(DEVICE_LOW_BATTERY_GRACE, 8U);
        (void)snprintf(name, sizeof(name), "%s_critical_battery_%s_grace", language, workflow_scenes[i].scene);
        capture(name, false);
        set_battery_phase(DEVICE_LOW_BATTERY_SHUTDOWN, 0U);
        (void)snprintf(name, sizeof(name), "%s_critical_battery_%s_stopping", language, workflow_scenes[i].scene);
        capture(name, false);
        set_battery_phase(DEVICE_LOW_BATTERY_NORMAL, 0U);
        (void)snprintf(name, sizeof(name), "%s_critical_battery_%s_cleared", language, workflow_scenes[i].scene);
        capture(name, false);
    }
    /*
     * The status read is non-blocking and fails while another task holds the status mutex. A
     * critical battery that was confirmed a moment ago must not vanish from the screen because of
     * that; a screen that never saw a critical state must not invent one.
     */
    prepare(WORKFLOW_STEP_RANGING, 4U);
    set_battery_phase(DEVICE_LOW_BATTERY_SHUTDOWN, 0U);
    render_device_status_result = ESP_ERR_TIMEOUT;
    ui_refresh_header_locked();
    (void)snprintf(name, sizeof(name), "%s_critical_battery_ranging_stopping_after_read_failure", language);
    capture(name, false);
    render_device_status_result = ESP_OK;

    prepare(WORKFLOW_STEP_RANGING, 4U);
    set_battery_phase(DEVICE_LOW_BATTERY_GRACE, 8U);
    render_device_status_result = ESP_ERR_TIMEOUT;
    ui_refresh_header_locked();
    (void)snprintf(name, sizeof(name), "%s_critical_battery_ranging_grace_after_read_failure", language);
    capture(name, false);
    render_device_status_result = ESP_OK;

    prepare(WORKFLOW_STEP_RANGING, 4U);
    set_battery_phase(DEVICE_LOW_BATTERY_NORMAL, 0U);
    render_device_status_result = ESP_ERR_TIMEOUT;
    ui_refresh_header_locked();
    (void)snprintf(name, sizeof(name), "%s_critical_battery_ranging_cleared_after_read_failure", language);
    capture(name, false);
    render_device_status_result = ESP_OK;

    /* USB came back: the next good read clears the bar for good. */
    prepare(WORKFLOW_STEP_RANGING, 4U);
    set_battery_phase(DEVICE_LOW_BATTERY_GRACE, 8U);
    render_device_status_result = ESP_ERR_TIMEOUT;
    ui_refresh_header_locked();
    render_device_status_result = ESP_OK;
    set_battery_phase(DEVICE_LOW_BATTERY_NORMAL, 0U);
    (void)snprintf(name, sizeof(name), "%s_critical_battery_ranging_cleared_after_recovery", language);
    capture(name, false);

    /* A screen that is not the workflow: the same device status, the same bar. */
    ui_workflow_screen_close_locked();
    memset(&render_device_status, 0, sizeof(render_device_status));
    render_context.screen = UI_SCREEN_HOME;
    ui_render_home_locked();
    set_battery_phase(DEVICE_LOW_BATTERY_SHUTDOWN, 0U);
    (void)snprintf(name, sizeof(name), "%s_critical_battery_home_stopping", language);
    capture(name, false);
    memset(&render_device_status, 0, sizeof(render_device_status));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    output_dir = argv[1];
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/frames.jsonl", output_dir);
    frames = fopen(path, "wb");
    assert(frames != NULL);
    lv_init();
    lv_display_t *display = lv_test_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    render_context.display = display;
    for (unsigned locale = 0U; locale < 2U; ++locale) {
        ui_i18n_set_locale(locale == 0U ? UI_LOCALE_EN : UI_LOCALE_UK);
        test_unchanged_sensor_cards_do_not_redraw();
        menu_family = "home";
        const char *language = locale == 0U ? "en" : "uk";
        ui_workflow_screen_close_locked();
        render_context.screen = UI_SCREEN_HOME;
        ui_render_home_locked();
        char name[96];
        (void)snprintf(name, sizeof(name), "%s_home", language);
        capture(name, true);
        for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
            for (unsigned remotes = 0U; remotes <= 4U; ++remotes) {
                prepare(step, remotes);
                if (step == WORKFLOW_STEP_RESULT) inject_result();
                const size_t actions = s_workflow->model.action_count;
                for (size_t selected = 0U; selected < (actions > 0U ? actions : 1U); ++selected) {
                    if (selected > 0U) ui_workflow_screen_handle_key_locked(KEYPAD_KEY_DOWN);
                    assert(s_workflow->model.selected_action == selected);
                    (void)snprintf(name, sizeof(name), "%s_%s_r%u_a%u", language,
                        workflow_core_step_name(step), remotes, (unsigned)selected);
                    capture(name, remotes == 4U ||
                        (step == WORKFLOW_STEP_SENSOR_CHECK && remotes < 3U && selected == 0U));
                }
                if (actions > 1U) {
                    ui_workflow_screen_handle_key_locked(KEYPAD_KEY_UP);
                    assert(s_workflow->model.selected_action == actions - 2U);
                }
            }
        }
        prepare(WORKFLOW_STEP_CALCULATING, 4U);
        s_workflow->snapshot.progress = 37;
        ui_workflow_update(s_workflow, render_now_ms);
        (void)snprintf(name, sizeof(name), "%s_calculating_progress", language);
        capture(name, true);
        test_ranging_progress_scene(language, "", 5U, 10U);          /* 10 of 20: 50% */
        test_ranging_progress_scene(language, "_3_nodes", 3U, 3U);   /* 3 of 6: 50% */
        test_ranging_progress_scene(language, "_4_nodes", 4U, 3U);   /* 3 of 12: 25% */
        test_ranging_progress_scene(language, "_5_nodes", 5U, 3U);   /* 3 of 20: 15% */
        test_ranging_wifi_screens(language);
        test_calibration_result_selection_after_run(language);
        test_placement_blocked_screens(language);
        test_ranging_result_screens(language);
        test_critical_battery_screens(language);
        prepare(WORKFLOW_STEP_STOPPED, 4U);
        s_workflow->model_state.material_view = UI_WORKFLOW_MATERIAL_SELECT;
        ui_workflow_update(s_workflow, render_now_ms);
        (void)snprintf(name, sizeof(name), "%s_material", language);
        capture(name, true);
        for (unsigned remotes = 0U; remotes < 3U; ++remotes) {
            prepare(WORKFLOW_STEP_SENSOR_CHECK, remotes);
            const unsigned before = render_command_count;
            ui_workflow_screen_handle_key_locked(KEYPAD_KEY_OK);
            assert(render_command_count == before + 1U && render_last_command == WORKFLOW_COMMAND_GO_CALIBRATION);
            (void)snprintf(name, sizeof(name), "%s_init_pending_r%u", language, remotes);
            capture(name, true);
            workflow_core_t completed;
            workflow_core_init(&completed, true);
            completed.step = WORKFLOW_STEP_SENSOR_CHECK;
            completed.generation = render_snapshot.generation;
            const workflow_command_params_t params = {
                .has_generation = true, .expected_generation = render_snapshot.generation,
            };
            assert(workflow_core_apply(&completed, WORKFLOW_COMMAND_GO_CALIBRATION,
                &params, remotes, false) == WORKFLOW_COMMAND_RESULT_OK);
            workflow_core_snapshot(&completed, remotes, false, &render_snapshot);
            render_snapshot.last_async = (workflow_last_async_t){
                .seq = s_workflow->model_state.pending_seq,
                .command = WORKFLOW_COMMAND_GO_CALIBRATION,
                .result = WORKFLOW_COMMAND_RESULT_OK,
            };
            const uint64_t before_snapshot_retry_ms = render_now_ms;
            render_now_ms += UI_WORKFLOW_REFRESH_PERIOD_MS;
            render_workflow_snapshot_result = ESP_ERR_TIMEOUT;
            ui_workflow_screen_refresh_locked();
            assert(s_workflow->snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
            render_workflow_snapshot_result = ESP_OK;
            ++render_now_ms;
            ui_workflow_screen_refresh_locked();
            assert(s_workflow->snapshot.step == WORKFLOW_STEP_CALIB_PREP);
            assert(!s_workflow->model_state.has_pending);
            (void)snprintf(name, sizeof(name), "%s_calib_prep_after_busy_r%u", language, remotes);
            capture(name, false);
            render_now_ms = before_snapshot_retry_ms;
        }
        prepare(WORKFLOW_STEP_OBJECT, 1U);
        ui_workflow_screen_handle_key_locked(KEYPAD_KEY_LEFT);
        assert(render_context.screen == UI_SCREEN_HOME);
        const workflow_step_t connection_steps[] = {
            WORKFLOW_STEP_OBJECT, WORKFLOW_STEP_SENSOR_CHECK, WORKFLOW_STEP_CALIB_PREP,
        };
        for (size_t step_index = 0U; step_index < sizeof(connection_steps) / sizeof(connection_steps[0]); ++step_index) {
            for (unsigned remotes = 0U; remotes <= 1U; ++remotes) {
                prepare(connection_steps[step_index], remotes);
                for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
                    render_device_status.pods[i].state = POD_STATE_WARNING;
                }
                ui_workflow_update(s_workflow, render_now_ms);
                for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
                    assert(s_workflow->remote_pod_states[i] ==
                        (i < remotes ? POD_STATE_WARNING : POD_STATE_DISCONNECTED));
                }
                (void)snprintf(name, sizeof(name), "%s_confirmed_links_step%u_r%u", language,
                    (unsigned)connection_steps[step_index], remotes);
                capture(name, false);
            }
        }
        prepare(WORKFLOW_STEP_SENSOR_CHECK, 0U);
        assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N+") != NULL);
        for (size_t i = 0U; i < UI_WORKFLOW_REMOTE_SLOT_COUNT; ++i) {
            assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[i]), "N- I? B? C-") != NULL);
        }
        (void)snprintf(name, sizeof(name), "%s_preflight_main_only", language);
        capture(name, false);
        prepare(WORKFLOW_STEP_SENSOR_CHECK, 1U);
        render_device_status.main.bmp280_present = true;
        render_device_status.main.bmp280_valid = true;
        render_device_status.power.valid = true;
        render_device_status.power.channels[2].valid = true;
        render_device_status.power.channels[2].voltage_v = 5.02f;
        render_device_status.pods[0].sensor_health = (device_sensor_health_t){
            .current = true, .imu_valid = true, .barometer_valid = true,
        };
        ui_workflow_update(s_workflow, render_now_ms);
        assert(s_workflow->remote_slot_cards[0] != NULL);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N+ I+ B+ C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_uncalibrated", language);
        capture(name, false);

        /*
         * The cards alternate between link status and measured values every
         * UI_WORKFLOW_SENSOR_PAGE_MS. Show the values page: pressure in hPa,
         * temperature in degC and the gyroscope rate in deg/s.
         */
        {
            const uint64_t status_page_ms = render_now_ms;
            uint64_t values_page_ms = status_page_ms - status_page_ms % UI_WORKFLOW_SENSOR_PAGE_MS;
            if (!ui_workflow_sensor_values_page(values_page_ms)) values_page_ms += UI_WORKFLOW_SENSOR_PAGE_MS;
            assert(ui_workflow_sensor_values_page(values_page_ms));
            assert(values_page_ms - status_page_ms < NRF_NODE_LINK_STATUS_STALE_LIMIT_MS);

            render_device_status.main.pressure_pa = 101010U;
            render_device_status.main.temperature_mc = 24140;
            render_device_status.pods[0].barometer.valid = true;
            render_device_status.pods[0].barometer.pressure_pa = 101010.0f;
            render_device_status.pods[0].environment.temperature_valid = true;
            render_device_status.pods[0].environment.temperature_c = 24.14f;
            render_device_status.pods[0].imu.gyro_valid = true;
            render_device_status.pods[0].imu.gyro_rate_dps = 0.26f;

            render_now_ms = values_page_ms;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strcmp(lv_label_get_text(s_workflow->remote_slot_labels[0]), "G1 1010.1 24.1 0.3") == 0);
            assert(strcmp(lv_label_get_text(s_workflow->remote_slot_labels[1]), "G2 -- -- --") == 0);
            assert(strstr(lv_label_get_text(render_context.footer_label), "1010.1 24.1") != NULL);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN") != NULL);
            (void)snprintf(name, sizeof(name), "%s_sensor_values", language);
            capture(name, true);

            /* Back on the status page nothing of the values leaks into the cards. */
            render_now_ms = status_page_ms;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N+ I+ B+ C-") != NULL);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N+") != NULL);

            /*
             * No nRF MAIN (4 REMOTE mode): the footer says so instead of a "not connected" mark for a part that is not
             * fitted. Anything uncertain keeps the usual MAIN link mark; MAIN present is unchanged.
             */
            render_topology_mode = NRF_TOPOLOGY_MODE_REMOTE_ONLY;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN nRF:") != NULL);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N") == NULL);
            assert(strstr(lv_label_get_text(render_context.footer_label), "INA:") != NULL);
            render_topology_mode = NRF_TOPOLOGY_MODE_UNCERTAIN;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N+") != NULL);
            render_topology_mode = NRF_TOPOLOGY_MODE_MAIN_PRESENT;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N+") != NULL);

            /* Three nodes: full, partial (no gyroscope) and none. */
            prepare(WORKFLOW_STEP_SENSOR_CHECK, 3U);
            for (size_t i = 0U; i < 3U; ++i) {
                render_device_status.pods[i].sensor_health = (device_sensor_health_t){
                    .current = true, .imu_valid = i != 1U, .imu_fault = i == 1U, .barometer_valid = i != 2U,
                };
            }
            render_device_status.pods[0].barometer.valid = true;
            render_device_status.pods[0].barometer.pressure_pa = 100000.0f;
            render_device_status.pods[0].environment.temperature_valid = true;
            render_device_status.pods[0].environment.temperature_c = -5.5f;
            render_device_status.pods[0].imu.gyro_valid = true;
            render_device_status.pods[0].imu.gyro_rate_dps = 12.0f;
            render_device_status.pods[1].barometer.valid = true;
            render_device_status.pods[1].barometer.pressure_pa = 99950.0f;
            render_device_status.pods[1].environment.temperature_valid = true;
            render_device_status.pods[1].environment.temperature_c = 21.0f;
            render_device_status.pods[1].imu.gyro_valid = false;
            render_device_status.pods[2].barometer.valid = false;
            render_device_status.pods[2].environment.temperature_valid = false;
            render_device_status.pods[2].imu.gyro_valid = false;
            render_now_ms = values_page_ms;
            ui_workflow_update(s_workflow, render_now_ms);
            assert(strcmp(lv_label_get_text(s_workflow->remote_slot_labels[0]), "G1 1000.0 -5.5 12.0") == 0);
            assert(strcmp(lv_label_get_text(s_workflow->remote_slot_labels[1]), "G2 999.5 21.0 --") == 0);
            assert(strcmp(lv_label_get_text(s_workflow->remote_slot_labels[2]), "G3 -- -- --") == 0);
            (void)snprintf(name, sizeof(name), "%s_sensor_values_partial", language);
            capture(name, true);
            render_now_ms = status_page_ms;
            memset(&render_device_status.pods[0].barometer, 0, sizeof(render_device_status.pods[0].barometer));
            memset(&render_device_status.pods[1].barometer, 0, sizeof(render_device_status.pods[1].barometer));
            memset(&render_device_status.pods[0].environment, 0, sizeof(render_device_status.pods[0].environment));
            memset(&render_device_status.pods[1].environment, 0, sizeof(render_device_status.pods[1].environment));
            memset(&render_device_status.pods[0].imu, 0, sizeof(render_device_status.pods[0].imu));
            prepare(WORKFLOW_STEP_SENSOR_CHECK, 1U);
            render_device_status.main.bmp280_present = true;
            render_device_status.main.bmp280_valid = true;
            render_device_status.pods[0].sensor_health = (device_sensor_health_t){
                .current = true, .imu_valid = true, .barometer_valid = true,
            };
            render_device_status.main.pressure_pa = 0U;
            render_device_status.main.temperature_mc = 0;
            ui_workflow_update(s_workflow, render_now_ms);
        }
        render_device_status.pods[0].sensor_health.imu_valid = false;
        render_device_status.pods[0].sensor_health.imu_fault = true;
        ui_workflow_update(s_workflow, render_now_ms);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "I! B+ C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_imu_fault", language);
        capture(name, false);
        render_device_status.pods[0].sensor_health.current = false;
        ui_workflow_update(s_workflow, render_now_ms);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "I? B? C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_stale", language);
        capture(name, false);
        render_device_status.pods[0].sensor_health.current = true;
        render_device_status.pods[0].sensor_health.imu_valid = true;
        render_device_status.pods[0].sensor_health.imu_fault = false;
        render_node_status[1].lifecycle = NRF_UPSTREAM_LIFECYCLE_BOOTING;
        ui_workflow_update(s_workflow, render_now_ms);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N! I? B? C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_nrf_booting", language);
        capture(name, false);
        render_node_status[1].lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
        const uint64_t before_stale_test_ms = render_now_ms;
        render_now_ms += NRF_NODE_LINK_STATUS_STALE_LIMIT_MS;
        ui_workflow_update(s_workflow, render_now_ms);
        /* A quiet link with a known identity is STALE (33907fc1), shown as "!" rather than "not connected". */
        assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N!") != NULL);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N! I? B? C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_nrf_stale", language);
        capture(name, false);
        render_node_status[0].status_receipt_uptime_ms = render_now_ms;
        render_node_status[1].status_receipt_uptime_ms = render_now_ms;
        ui_workflow_update(s_workflow, render_now_ms);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N+ I+ B+ C-") != NULL);
        render_node_status[1].status_identity_matches = false;
        ui_workflow_update(s_workflow, render_now_ms);
        assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N+") != NULL);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N- I? B? C-") != NULL);
        (void)snprintf(name, sizeof(name), "%s_preflight_nrf_identity_mismatch", language);
        capture(name, false);
        render_now_ms = before_stale_test_ms;
        prepare(WORKFLOW_STEP_SENSOR_CHECK, 3U);
        render_device_status.pods[1].state = POD_STATE_WARNING;
        render_device_status.pods[2].state = POD_STATE_ERROR;
        ui_workflow_update(s_workflow, render_now_ms);
        (void)snprintf(name, sizeof(name), "%s_remote_status", language);
        capture(name, true);
        prepare(WORKFLOW_STEP_SENSOR_CHECK, 1U);
        (void)snprintf(name, sizeof(name), "%s_long_text_start", language);
        capture(name, true);
        lv_test_fast_forward(2200U);
        (void)snprintf(name, sizeof(name), "%s_long_text_scrolled", language);
        capture(name, true);
        ui_workflow_screen_close_locked();
        memset(render_node_status, 0, sizeof(render_node_status));
        render_all_menus();
    }
    ui_workflow_screen_close_locked();
    fclose(frames);
    printf("REAL_LVGL_RENDER %u.%u.%u RGB565 320x240: %u frames; keypad and low-count commands PASS\n",
        LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, frame_count);
    lv_deinit();
    return 0;
}
#endif
