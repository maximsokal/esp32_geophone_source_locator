/* Real application-key dispatch and registry; only service/RTOS boundaries are fake. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_navigation_fixture.h"
#include "src/display/lv_display_private.h"

static FILE *journal;
static unsigned action_index, capture_index;
static uint32_t seed;
static const char *scenario;
static uint32_t home_screen_count;

static void checkpoint(const char *name)
{
    char filename[128];
    (void)snprintf(filename, sizeof(filename), "%s_%s_%03u_%s",
        ui_i18n_get_locale() == UI_LOCALE_EN ? "en" : "uk", scenario, capture_index++, name);
    render_navigation_capture(filename);
}

static void check_lifecycle(void)
{
    assert(render_context.screen >= UI_SCREEN_HOME && render_context.screen < UI_SCREEN_COUNT);
    assert(lv_obj_is_valid(render_context.root));
    assert(lv_display_get_screen_active(render_context.display) == render_context.root);
    assert(render_context.display->screen_cnt == home_screen_count);
    assert(lv_mem_test() == LV_RESULT_OK);
    if (render_context.screen != UI_SCREEN_WORKFLOW && render_context.screen != UI_SCREEN_SCAN_PREVIEW) {
        assert(!render_workflow_exists());
    }
    assert(render_preview_open() == (render_context.screen == UI_SCREEN_SCAN_PREVIEW));
    assert(render_context.selected < ui_screen_home_count());
    assert(render_context.object_count <= OBJECT_SERVICE_MAX_OBJECTS);
    if (render_context.object_count > 0U) assert(render_context.object_index < render_context.object_count);
}

static void key(keypad_key_t value)
{
    const ui_screen_t before = render_context.screen;
    fprintf(journal, "{\"phase\":\"before\",\"index\":%u,\"locale\":%d,\"seed\":%lu,\"key\":%d,\"screen\":%d}\n",
        action_index, (int)ui_i18n_get_locale(), (unsigned long)seed, (int)value, (int)before);
    fflush(journal);
    /* Same boundary called by ui_event_dispatcher after delivery/staleness checks. */
    ui_dispatch_key_locked(value);
    render_now_ms += 25U;
    lv_test_fast_forward(25U);
    ui_screen_refresh_locked(render_context.screen);
    fprintf(journal, "{\"phase\":\"after\",\"index\":%u,\"screen\":%d,\"selected\":%u,\"allocations\":%u,\"bytes\":%zu}\n",
        action_index++, (int)render_context.screen, (unsigned)render_context.selected,
        render_heap_allocations, render_heap_bytes);
    fflush(journal);
    check_lifecycle();
}

static void home(void)
{
    fprintf(journal, "{\"phase\":\"route_home\",\"index\":%u}\n", action_index);
    fflush(journal);
    ui_open_home_locked();
    check_lifecycle();
}

static void enter(ui_screen_t screen)
{
    home();
    size_t index = 0U;
    while (index < ui_screen_home_count() && ui_screen_home_at(index) != screen) ++index;
    assert(index < ui_screen_home_count());
    for (size_t i = 0U; i < index; ++i) key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
}

static void snapshot(workflow_step_t step, unsigned remotes, bool active)
{
    workflow_core_t core;
    workflow_core_init(&core, true);
    core.step = step;
    core.calibration_ok = core.ranging_ok = core.calc_settings_ready = true;
    workflow_core_snapshot(&core, remotes, true, &render_snapshot);
    render_snapshot.generation = 17U;
    if (active) (void)snprintf(render_snapshot.object_id, sizeof(render_snapshot.object_id), "O0");
    (void)snprintf(render_snapshot.session_id, sizeof(render_snapshot.session_id), "S0");
    memset(&render_measurement, 0, sizeof(render_measurement));
    render_measurement.state = MEASUREMENT_SERVICE_LIVE;
    render_measurement.active_object_present = active;
    render_service_result = render_acquisition_result = ESP_OK;
    render_service_busy = false;
    render_storage_ready = render_wifi_saved = true;
}

static void list_done(bool active)
{
    object_service_summary_t objects[2] = {0};
    for (unsigned i = 0U; i < 2U; ++i) {
        (void)snprintf(objects[i].id, sizeof(objects[i].id), "O%u", i);
        (void)snprintf(objects[i].name, sizeof(objects[i].name), "Bridge %u", i);
        objects[i].active = active && i == 0U;
    }
    object_service_event_t event = {.type = OBJECT_SERVICE_EVENT_LIST_DONE, .object_count = 2U};
    memcpy(event.objects, objects, sizeof(objects));
    ui_object_service_event(&event, NULL);
    render_active_object = active;
}

static void activation_done(void)
{
    object_service_event_t event = {.type = OBJECT_SERVICE_EVENT_SET_ACTIVE_DONE};
    (void)snprintf(event.object_id, sizeof(event.object_id), "O0");
    ui_object_service_event(&event, NULL);
}

static void assert_workflow_memory_notice(void)
{
    const char *message = ui_tr(UI_I18N_KEY_TFT_WORKFLOW_MEMORY_UNAVAILABLE);
    for (uint32_t i = 0U; i < lv_obj_get_child_count(render_context.root); ++i) {
        lv_obj_t *child = lv_obj_get_child(render_context.root, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            !lv_obj_is_hidden(child) &&
            strcmp(lv_label_get_text(child), message) == 0) return;
    }
    assert(false && "Workflow allocation failure must be visible to the operator");
}

static void workflow_memory_failure(void)
{
    snapshot(WORKFLOW_STEP_OBJECT, 1U, true);
    home();
    const unsigned starts = render_acquisition_starts;
    const unsigned commands = render_command_count;
    render_heap_failures_remaining = 1U;
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_heap_failures_remaining == 0U);
    assert(render_context.screen == UI_SCREEN_OBJECTS && !render_workflow_exists());
    assert(render_heap_allocations == 0U);
    assert(render_acquisition_starts == starts && render_command_count == commands);
    assert_workflow_memory_notice();
    checkpoint("workflow_oom_loading");
    list_done(true);
    assert_workflow_memory_notice();
    checkpoint("workflow_oom_loaded");
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_context.screen == UI_SCREEN_WORKFLOW && render_workflow_exists());
    assert(render_acquisition_starts == starts && render_command_count == commands);
    checkpoint("workflow_oom_retry");
    home();
    assert(render_heap_allocations == 0U);
}

static void entry_routes(void)
{
    snapshot(WORKFLOW_STEP_RECORD_READY, 1U, true);
    home();
    key(KEYPAD_KEY_UP);
    assert(render_context.selected == ui_screen_home_count() - 1U);
    key(KEYPAD_KEY_DOWN);
    assert(render_context.selected == 0U);
    key(KEYPAD_KEY_NONE);
    checkpoint("home");
    enter(UI_SCREEN_OBJECTS);
    assert(render_context.screen == UI_SCREEN_OBJECTS && !render_context.objects_loaded);
    list_done(true);
    checkpoint("objects_loaded");
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_RIGHT);
    assert(render_context.objects_mode == UI_OBJECTS_SELECT_ACTIVE);
    key(KEYPAD_KEY_LEFT);
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_HOME);
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_context.screen == UI_SCREEN_MEASUREMENTS);
    assert(render_acquisition_starts > 0U);
    checkpoint("measurements");
    key(KEYPAD_KEY_LEFT);
    enter(UI_SCREEN_SYSTEM);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    assert(render_context.system_hardware_open);
    checkpoint("hardware");
    key(KEYPAD_KEY_LEFT);
    key(KEYPAD_KEY_LEFT);
    enter(UI_SCREEN_SYSTEM);
    assert(!render_context.system_hardware_open && !render_context.system_hardware_selected);
    checkpoint("system_reentered");
    key(KEYPAD_KEY_LEFT);
    for (unsigned remotes = 0U; remotes < 3U; ++remotes) {
        snapshot(WORKFLOW_STEP_SENSOR_CHECK, remotes, true);
        const unsigned commands = render_command_count;
        enter(UI_SCREEN_MEASUREMENTS);
        /* Sensor check offers the live view for listening only; it sends no workflow command. */
        assert(render_context.screen == UI_SCREEN_MEASUREMENTS && render_context.measurement_listen_only);
        assert(render_command_count == commands);
        key(KEYPAD_KEY_LEFT);
        /* Leaving it returns to the step it was opened from, not to HOME. */
        assert(render_context.screen == UI_SCREEN_WORKFLOW);
        assert(render_command_count == commands);
        key(KEYPAD_KEY_OK);
        assert(render_command_count == commands + 1U && render_last_command == WORKFLOW_COMMAND_GO_CALIBRATION);
        checkpoint("initialize_fewer_than_three");
    }
    snapshot(WORKFLOW_STEP_SENSOR_CHECK, 0U, true);
    render_acquisition_result = ESP_ERR_INVALID_STATE;
    enter(UI_SCREEN_MEASUREMENTS);
    /* The live view opens even when the ADC cannot start (it then shows the unavailable state) and
     * hands the operator back to the workflow. */
    assert(render_context.screen == UI_SCREEN_MEASUREMENTS && render_context.measurement_listen_only);
    checkpoint("acquisition_unavailable");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_WORKFLOW);
    home();
}

static void picker_route(void)
{
    snapshot(WORKFLOW_STEP_IDLE, 1U, false);
    render_active_object = false;
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_context.screen == UI_SCREEN_OBJECTS);
    assert(render_context.objects_mode == UI_OBJECTS_SELECT_WORKFLOW);
    checkpoint("picker_loading");
    list_done(true);
    checkpoint("picker_listed");
    assert(render_context.objects_mode == UI_OBJECTS_SELECT_WORKFLOW);
    key(KEYPAD_KEY_OK);
    assert(render_context.screen == UI_SCREEN_WORKFLOW);
    checkpoint("workflow_from_picker");
    home();
}

static void activation_route(void)
{
    snapshot(WORKFLOW_STEP_OBJECT, 1U, true);
    enter(UI_SCREEN_OBJECTS);
    list_done(false);
    for (unsigned i = 0U; i < 3U; ++i) key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    key(KEYPAD_KEY_OK);
    assert(render_context.workflow_activation_pending);
    checkpoint("activation_pending");
    key(KEYPAD_KEY_LEFT);
    activation_done();
    assert(render_context.screen == UI_SCREEN_OBJECTS && !render_context.workflow_activation_pending);
    checkpoint("activation_cancelled");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_HOME);
    activation_done();
    checkpoint("activation_after_back");
    assert(render_context.screen == UI_SCREEN_HOME && !render_context.workflow_activation_pending);
    enter(UI_SCREEN_OBJECTS);
    list_done(false);
    for (unsigned i = 0U; i < 3U; ++i) key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    key(KEYPAD_KEY_OK);
    activation_done();
    assert(render_context.screen == UI_SCREEN_WORKFLOW && !render_context.workflow_activation_pending);
    checkpoint("activation_current");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_HOME);
}

static void preview_routes(void)
{
    snapshot(WORKFLOW_STEP_RESULT, 1U, true);
    render_snapshot.has_scan = true;
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_context.screen == UI_SCREEN_WORKFLOW);
    const uint32_t result_generation = render_workflow_result_generation();
    key(KEYPAD_KEY_RIGHT);
    assert(render_context.screen == UI_SCREEN_SCAN_PREVIEW && render_workflow_exists());
    checkpoint("preview_loading");
    render_preview_complete(true);
    render_preview_complete(false);
    checkpoint("preview_completed");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_WORKFLOW);
    assert(render_workflow_result_generation() == result_generation);
    checkpoint("preview_back");
    key(KEYPAD_KEY_RIGHT);
    home();
    assert(!render_workflow_exists() && render_heap_allocations == 0U);
    render_preview_complete(true);
    check_lifecycle();
    checkpoint("preview_to_home");
    snapshot(WORKFLOW_STEP_RECORD_READY, 1U, true);
    render_measurement.state = MEASUREMENT_SERVICE_REVIEW;
    (void)snprintf(render_measurement.object_id, sizeof(render_measurement.object_id), "O0");
    (void)snprintf(render_measurement.session_id, sizeof(render_measurement.session_id), "S0");
    enter(UI_SCREEN_MEASUREMENTS);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    assert(render_context.screen == UI_SCREEN_SCAN_PREVIEW && !render_workflow_exists());
    checkpoint("measurement_preview");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.screen == UI_SCREEN_HOME);
}

static void close_during_result(void) { home(); }
static void reopen_during_result(void)
{
    home();
    snapshot(WORKFLOW_STEP_OBJECT, 1U, true);
    enter(UI_SCREEN_MEASUREMENTS);
}

static void result_ownership(void)
{
    snapshot(WORKFLOW_STEP_RESULT, 1U, true);
    enter(UI_SCREEN_MEASUREMENTS);
    assert(render_workflow_result_pending() && !render_workflow_result_ready());
    checkpoint("result_pending");
    render_run_task("workflow-result", NULL);
    assert(!render_workflow_result_pending() && render_workflow_result_ready());
    render_now_ms += 500U;
    ui_screen_refresh_locked(render_context.screen);
    checkpoint("result_current_complete");
    home();
    snapshot(WORKFLOW_STEP_RESULT, 1U, true);
    enter(UI_SCREEN_MEASUREMENTS);
    render_run_task("workflow-result", close_during_result);
    assert(render_context.screen == UI_SCREEN_HOME && render_heap_allocations == 0U);
    checkpoint("result_closed_complete");
    snapshot(WORKFLOW_STEP_RESULT, 1U, true);
    enter(UI_SCREEN_MEASUREMENTS);
    render_run_task("workflow-result", reopen_during_result);
    assert(render_context.screen == UI_SCREEN_WORKFLOW && !render_workflow_result_ready());
    assert(render_workflow_result_generation() == 0U);
    checkpoint("result_reopened_stale_ignored");
    home();
}

static void confirmations(void)
{
    snapshot(WORKFLOW_STEP_OBJECT, 1U, true);
    enter(UI_SCREEN_STORAGE);
    assert(render_context.storage_action == 0U && !render_context.storage_erase_confirm);
    storage_service_event_t info = {.type = STORAGE_SERVICE_EVENT_INFO_DONE};
    (void)snprintf(info.info.name, sizeof(info.info.name), "TestSD");
    (void)snprintf(info.info.type, sizeof(info.info.type), "SDHC");
    ui_storage_service_event(&info, NULL);
    const unsigned erased = render_erase_calls;
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    assert(render_context.storage_erase_confirm && render_erase_calls == erased);
    checkpoint("erase_armed");
    key(KEYPAD_KEY_LEFT);
    enter(UI_SCREEN_STORAGE);
    assert(render_context.storage_action == 0U && !render_context.storage_erase_confirm);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_RIGHT);
    key(KEYPAD_KEY_OK);
    assert(render_erase_calls == erased + 1U && !render_context.storage_erase_confirm);
    checkpoint("erase_confirmed");
    enter(UI_SCREEN_WIFI);
    const unsigned setup = render_wifi_setup_calls;
    key(KEYPAD_KEY_OK);
    assert(render_context.wifi_confirm == UI_WIFI_CONFIRM_SETUP && render_wifi_setup_calls == setup);
    checkpoint("wifi_armed");
    key(KEYPAD_KEY_LEFT);
    enter(UI_SCREEN_WIFI);
    assert(render_context.wifi_confirm == UI_WIFI_CONFIRM_NONE && render_context.wifi_action == 0U);
    key(KEYPAD_KEY_OK);
    key(KEYPAD_KEY_RIGHT);
    assert(render_wifi_setup_calls == setup + 1U);
    checkpoint("wifi_confirmed");
    enter(UI_SCREEN_OBJECTS);
    list_done(true);
    const unsigned deleted = render_delete_calls;
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_DOWN);
    key(KEYPAD_KEY_OK);
    key(KEYPAD_KEY_OK);
    assert(render_context.objects_mode == UI_OBJECTS_DELETE_CONFIRM && render_delete_calls == deleted);
    checkpoint("delete_armed");
    key(KEYPAD_KEY_LEFT);
    assert(render_context.objects_mode == UI_OBJECTS_SELECT_DELETE && render_delete_calls == deleted);
    key(KEYPAD_KEY_OK);
    key(KEYPAD_KEY_RIGHT);
    assert(render_delete_calls == deleted + 1U);
    checkpoint("delete_confirmed");
    home();
}

static size_t timer_count(void)
{
    size_t count = 0U;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer != NULL; timer = lv_timer_get_next(timer)) ++count;
    return count;
}

static void repeated_lifecycle(void)
{
    size_t timers = 0U, blocks = 0U;
    for (unsigned cycle = 0U; cycle < 33U; ++cycle) {
        snapshot(WORKFLOW_STEP_RESULT, cycle % 5U, true);
        render_snapshot.has_scan = true;
        enter(UI_SCREEN_MEASUREMENTS);
        key(KEYPAD_KEY_RIGHT);
        key(KEYPAD_KEY_LEFT);
        home();
        render_run_task("workflow-result", NULL);
        assert(render_heap_allocations == 0U && render_heap_bytes == 0U);
        lv_obj_update_layout(render_context.root);
        lv_refr_now(render_context.display);
        lv_mem_monitor_t memory;
        lv_mem_monitor(&memory);
        fprintf(journal, "{\"phase\":\"memory\",\"cycle\":%u,\"timers\":%zu,\"free\":%zu,\"blocks\":%zu}\n",
            cycle, timer_count(), memory.free_size, memory.used_cnt);
        fflush(journal);
        if (cycle == 0U) { timers = timer_count(); blocks = memory.used_cnt; }
        else {
            assert(timer_count() == timers);
            /* TLSF block splitting changes free bytes even with identical live allocations. */
            assert(memory.used_cnt == blocks);
        }
        if (cycle == 0U || cycle == 32U) checkpoint(cycle == 0U ? "cycle_first" : "cycle_last");
    }
}

static void replay(unsigned actions)
{
    uint32_t random = seed;
    snapshot(WORKFLOW_STEP_OBJECT, 1U, true);
    home();
    for (unsigned i = 0U; i < actions; ++i) {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        key((keypad_key_t)(KEYPAD_KEY_LEFT + random % 5U));
        if (render_context.screen == UI_SCREEN_OBJECTS && !render_context.objects_loaded) list_done(true);
        if (i % 64U == 0U) checkpoint("seeded_keys");
    }
    home();
    checkpoint("replay_finished");
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    scenario = argv[2];
    seed = (uint32_t)strtoul(argv[3], NULL, 10);
    const unsigned actions = (unsigned)strtoul(argv[4], NULL, 10);
    assert(seed > 0U && actions > 0U && actions <= 2048U);
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/actions.jsonl", argv[1]);
    journal = fopen(path, "wb");
    assert(journal != NULL);
    render_navigation_start(argv[1]);
    atomic_store(&render_context.runtime_ready, true);
    ui_storage_query_gate_open();
    ui_open_home_locked();
    home_screen_count = render_context.display->screen_cnt;
    for (unsigned i = 0U; i < HW_MODULE_COUNT; ++i) render_modules[i] = (hw_module_info_t){
        .id = (hw_module_id_t)i, .state = HW_STATE_PRESENT, .requirement = HW_REQ_OPTIONAL};
    for (unsigned locale = 0U; locale < 2U; ++locale) {
        ui_i18n_set_locale(locale == 0U ? UI_LOCALE_EN : UI_LOCALE_UK);
        if (strcmp(scenario, "entry") == 0) { entry_routes(); workflow_memory_failure(); }
        else if (strcmp(scenario, "picker") == 0) picker_route();
        else if (strcmp(scenario, "activation") == 0) activation_route();
        else if (strcmp(scenario, "preview") == 0) preview_routes();
        else if (strcmp(scenario, "result") == 0) result_ownership();
        else if (strcmp(scenario, "confirm") == 0) confirmations();
        else if (strcmp(scenario, "lifecycle") == 0) repeated_lifecycle();
        else { assert(strcmp(scenario, "replay") == 0); replay(actions); }
        check_lifecycle();
    }
    home();
    assert(render_heap_allocations == 0U && render_heap_bytes == 0U);
    fclose(journal);
    render_navigation_finish();
    printf("REAL_LVGL_NAVIGATION %s seed=%lu actions=%u frames=%u PASS\n",
        scenario, (unsigned long)seed, action_index, capture_index);
    return 0;
}
