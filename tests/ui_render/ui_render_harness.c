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
extern nrf_node_link_status_input_t render_node_status[UI_WORKFLOW_REMOTE_SLOT_COUNT + 1U];
extern uint64_t render_now_ms;
extern workflow_command_t render_last_command;
extern unsigned render_command_count;
extern esp_err_t render_workflow_snapshot_result;
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
    render_snapshot.calibration.pairs_done = remotes * (remotes + 1U) / 2U;
    render_snapshot.calibration.pairs_total = render_snapshot.calibration.pairs_done;
    render_snapshot.placement.per_node_count = remotes + 1U;
    render_snapshot.placement.all_stable = true;
    for (size_t i = 0U; i < remotes + 1U; ++i) {
        workflow_calibration_node_t *node = &render_snapshot.calibration.per_node[i];
        (void)snprintf(node->node, sizeof(node->node), i == 0U ? "MAIN" : "G%u", (unsigned)i);
        (void)snprintf(node->height_state, sizeof(node->height_state), "ready");
        (void)snprintf(node->imu_state, sizeof(node->imu_state), "ready");
        node->relative_z_mm_valid = true;
        node->relative_z_mm = (int32_t)i * 12;
        node->cs_pairs_ok = true;
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
        prepare(WORKFLOW_STEP_RANGING, 4U);
        s_workflow->ranging.available = true;
        s_workflow->ranging.pairs_done = 3U;
        s_workflow->ranging.pairs_total = 10U;
        s_workflow->ranging.residual_valid = true;
        s_workflow->ranging.residual_mm = 12;
        s_workflow->ranging.node_count = 5U;
        for (size_t node = 0U; node < 5U; ++node) {
            (void)snprintf(s_workflow->ranging.nodes[node], sizeof(s_workflow->ranging.nodes[node]),
                node == 0U ? "MAIN" : "G%u", (unsigned)node);
            s_workflow->ranging.sigma_mm[node] = 10;
        }
        ui_workflow_update(s_workflow, render_now_ms);
        (void)snprintf(name, sizeof(name), "%s_ranging_progress", language);
        capture(name, true);
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
        assert(strstr(lv_label_get_text(render_context.footer_label), "MAIN N-") != NULL);
        assert(strstr(lv_label_get_text(s_workflow->remote_slot_labels[0]), "N- I? B? C-") != NULL);
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
