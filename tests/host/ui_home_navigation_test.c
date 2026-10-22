#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui_internal.h"
#include "ui_measurements_route.h"
#include "workflow_service.h"


static ui_context_t s_context;
static unsigned s_home_renders;
static unsigned s_objects_renders;
static unsigned s_measurement_renders;
static unsigned s_storage_renders;
static unsigned s_wifi_renders;
static unsigned s_system_renders;
static unsigned s_object_list_requests;
static unsigned s_storage_info_requests;
static unsigned s_acquisition_start_calls;
static esp_err_t s_acquisition_start_result;
static bool s_active_object_present;
static workflow_snapshot_t s_workflow_snapshot;
static unsigned char s_fake_lvgl_object;


ui_context_t *ui_context(void)
{
    return &s_context;
}


void ui_set_notice_locked(const char *text)
{
    (void)text;
}


void ui_refresh_header_locked(void)
{
}


void ui_render_home_locked(void)
{
    ++s_home_renders;
}


void ui_render_objects_locked(void)
{
    ++s_objects_renders;
}


void ui_render_measurements_locked(void)
{
    ++s_measurement_renders;
}


void ui_render_storage_locked(void)
{
    ++s_storage_renders;
}


void ui_render_wifi_locked(void)
{
    ++s_wifi_renders;
}


#if !defined(_MSC_VER)
void ui_workflow_screen_open_locked(void) {}
void ui_workflow_screen_close_locked(void) {}
void ui_workflow_screen_render_locked(void) {}
void ui_workflow_screen_refresh_locked(void) {}
void ui_workflow_screen_handle_key_locked(keypad_key_t key) { (void)key; }
void ui_scan_preview_refresh_locked(void) {}
void ui_home_refresh_locked(void) {}
void ui_render_hardware_locked(void) {}
#endif


void ui_measurements_refresh_locked(void)
{
}


static lv_obj_t *fake_lvgl_object(void)
{
    return (lv_obj_t *)&s_fake_lvgl_object;
}


void ui_handle_measurements_key_locked(keypad_key_t key)
{
    (void)key;
}


void ui_handle_storage_key_locked(keypad_key_t key)
{
    (void)key;
}


void ui_handle_wifi_key_locked(keypad_key_t key)
{
    (void)key;
}


esp_err_t object_service_list_async(void)
{
    ++s_object_list_requests;
    return ESP_OK;
}


esp_err_t object_service_get_active(object_service_summary_t *object)
{
    if (object == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_active_object_present) return ESP_FAIL;
    memset(object, 0, sizeof(*object));
    object->active = true;
    (void)snprintf(object->id, sizeof(object->id), "%s", "active-object");
    return ESP_OK;
}


bool object_service_is_busy(void)
{
    return false;
}


esp_err_t object_service_set_active_async(const char *object_id)
{
    return object_id != NULL && object_id[0] != '\0' ? ESP_OK : ESP_ERR_INVALID_ARG;
}


esp_err_t object_service_create_async(void)
{
    return ESP_OK;
}


esp_err_t object_service_delete_async(const char *object_id)
{
    return object_id != NULL && object_id[0] != '\0' ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t object_store_get_delete_scope(const char *object_id, object_store_delete_scope_t *scope)
{
    if (object_id == NULL || scope == NULL) return ESP_ERR_INVALID_ARG;
    memset(scope, 0, sizeof(*scope));
    return ESP_OK;
}


esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    memset(snapshot, 0, sizeof(*snapshot));
    return ESP_OK;
}


bool measurement_service_snapshot_allows_object_mutation(
    const measurement_service_snapshot_t *snapshot)
{
    (void)snapshot;
    return true;
}


esp_err_t workflow_service_try_get_snapshot(workflow_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = s_workflow_snapshot;
    return ESP_OK;
}


esp_err_t seismic_acquisition_start(void)
{
    ++s_acquisition_start_calls;
    return s_acquisition_start_result;
}


esp_err_t storage_service_info_async(void)
{
    ++s_storage_info_requests;
    return ESP_OK;
}


lv_obj_t *ui_create_screen_base_locked(const char *title, const char *footer)
{
    (void)title;
    (void)footer;
    ++s_system_renders;
    return fake_lvgl_object();
}


lv_obj_t *ui_create_card_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    int height
)
{
    (void)parent;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return fake_lvgl_object();
}


lv_obj_t *ui_create_label_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    (void)parent;
    (void)x;
    (void)y;
    (void)width;
    (void)text;
    (void)color;
    return fake_lvgl_object();
}


lv_obj_t *ui_create_row_locked(
    lv_obj_t *parent,
    int y,
    const char *text,
    bool selected,
    bool enabled
)
{
    (void)parent;
    (void)y;
    (void)text;
    (void)selected;
    (void)enabled;
    return fake_lvgl_object();
}


const char *lv_label_get_text(const lv_obj_t *label)
{
    (void)label;
    return "";
}


void lv_label_set_text(lv_obj_t *label, const char *text)
{
    (void)label;
    (void)text;
}


const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {"host"};
    return &app;
}


const char *esp_get_idf_version(void)
{
    return "host";
}


size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 1024U;
}


int64_t esp_timer_get_time(void)
{
    return 1000000LL;
}


esp_err_t ui_event_dispatcher_queue_locale_save(ui_locale_t locale)
{
    (void)locale;
    return ESP_OK;
}


void ui_scan_preview_screen_open_locked(void)
{
}


void ui_scan_preview_close_locked(void)
{
}


void ui_scan_preview_release_locked(void)
{
}


void ui_scan_preview_render_locked(void)
{
}


void ui_scan_preview_handle_key_locked(keypad_key_t key)
{
    (void)key;
}


static void reset_state(void)
{
    memset(&s_context, 0, sizeof(s_context));
    s_context.screen = UI_SCREEN_HOME;
    s_home_renders = 0U;
    s_objects_renders = 0U;
    s_measurement_renders = 0U;
    s_storage_renders = 0U;
    s_wifi_renders = 0U;
    s_system_renders = 0U;
    s_object_list_requests = 0U;
    s_storage_info_requests = 0U;
    s_acquisition_start_calls = 0U;
    s_acquisition_start_result = ESP_OK;
    s_active_object_present = false;
    memset(&s_workflow_snapshot, 0, sizeof(s_workflow_snapshot));
    s_workflow_snapshot.step = WORKFLOW_STEP_IDLE;
}


static void test_home_registry_mapping(void)
{
    assert(ui_screen_home_count() == 5U);
    assert(ui_screen_home_at(0U) == UI_SCREEN_OBJECTS);
    assert(ui_screen_home_at(1U) == UI_SCREEN_MEASUREMENTS);
    assert(ui_screen_home_at(2U) == UI_SCREEN_STORAGE);
    assert(ui_screen_home_at(3U) == UI_SCREEN_WIFI);
    assert(ui_screen_home_at(4U) == UI_SCREEN_SYSTEM);
    assert(ui_screen_home_at(5U) == UI_SCREEN_HOME);
    assert(strcmp(ui_screen_home_label(0U), "OBJECTS") == 0);
    assert(strcmp(ui_screen_home_label(4U), "SYSTEM") == 0);
}


static void test_home_up_down_wrap_and_render(void)
{
    reset_state();

    ui_screen_open_locked(UI_SCREEN_HOME);
    assert(s_context.selected == 0U);

    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_UP);
    assert(s_context.screen == UI_SCREEN_HOME);
    assert(s_context.selected == 4U);
    assert(s_home_renders == 2U);

    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_DOWN);
    assert(s_context.selected == 0U);
    assert(s_home_renders == 3U);
}


static void test_right_and_ok_open_selected_screen(void)
{
    reset_state();

    ui_screen_open_locked(UI_SCREEN_HOME);
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_RIGHT);
    assert(s_context.screen == UI_SCREEN_OBJECTS);
    assert(s_object_list_requests == 1U);
    assert(s_objects_renders == 1U);

    ui_screen_open_locked(UI_SCREEN_HOME);
    s_context.selected = 2U;
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_OK);
    assert(s_context.screen == UI_SCREEN_STORAGE);
    assert(s_storage_info_requests == 1U);
    assert(s_storage_renders == 1U);
}


static void test_wifi_entry_renders_the_wifi_screen(void)
{
    reset_state();
    ui_screen_open_locked(UI_SCREEN_HOME);
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_UP); /* 0 -> 4 */
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_UP); /* 4 -> 3 */
    assert(s_context.selected == 3U);
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_OK);
    assert(s_context.screen == UI_SCREEN_WIFI);
    assert(s_wifi_renders == 1U);
}


static void test_objects_entry_does_not_list_during_recovery(void)
{
    reset_state();
    s_context.storage_recovery_active = true;

    ui_screen_open_locked(UI_SCREEN_OBJECTS);

    assert(s_context.screen == UI_SCREEN_OBJECTS);
    assert(s_object_list_requests == 0U);
    assert(s_objects_renders == 1U);
}


static void test_left_from_system_returns_home(void)
{
    reset_state();

    ui_screen_open_locked(UI_SCREEN_SYSTEM);
    assert(s_context.screen == UI_SCREEN_SYSTEM);
    assert(s_system_renders == 1U);

    ui_screen_handle_key_locked(UI_SCREEN_SYSTEM, KEYPAD_KEY_LEFT);
    assert(s_context.screen == UI_SCREEN_HOME);
    assert(s_context.selected == 0U);
    assert(s_home_renders == 1U);
}


static void test_dispatch_uses_current_screen_driver(void)
{
    reset_state();
    s_active_object_present = true;
    s_workflow_snapshot.step = WORKFLOW_STEP_RECORD_READY;
    (void)snprintf(s_workflow_snapshot.object_id,
        sizeof(s_workflow_snapshot.object_id), "%s", "active-object");

    ui_screen_open_locked(UI_SCREEN_HOME);
    ui_dispatch_key_locked(KEYPAD_KEY_DOWN);
    assert(s_context.selected == 1U);

    ui_dispatch_key_locked(KEYPAD_KEY_OK);
    assert(s_context.screen == UI_SCREEN_MEASUREMENTS);
    assert(s_measurement_renders == 1U);
    assert(s_acquisition_start_calls == 1U);
}


static void test_measurements_route_decision_all_workflow_states(void)
{
    for (workflow_step_t step = WORKFLOW_STEP_IDLE; step < WORKFLOW_STEP_COUNT; ++step) {
        const bool allowed = step == WORKFLOW_STEP_RECORD_READY ||
            step == WORKFLOW_STEP_RECORDING;
        assert(ui_measurements_route_decide(step, true) ==
            (allowed ? UI_MEASUREMENTS_ROUTE_ALLOW : UI_MEASUREMENTS_ROUTE_WORKFLOW));
        assert(ui_measurements_route_decide(step, false) ==
            (allowed ? UI_MEASUREMENTS_ROUTE_ALLOW : UI_MEASUREMENTS_ROUTE_OBJECT_PICKER));
    }
}


static void test_home_measurements_redirects_active_object_for_ok_and_right(void)
{
    const keypad_key_t keys[] = { KEYPAD_KEY_OK, KEYPAD_KEY_RIGHT };
    for (size_t i = 0U; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        reset_state();
        s_active_object_present = true;
        s_workflow_snapshot.step = WORKFLOW_STEP_OBJECT;
        s_workflow_snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_START_MEASUREMENT);
        (void)snprintf(s_workflow_snapshot.object_id,
            sizeof(s_workflow_snapshot.object_id), "%s", "active-object");

        ui_screen_open_locked(UI_SCREEN_HOME);
        s_context.selected = 1U;
        ui_screen_handle_key_locked(UI_SCREEN_HOME, keys[i]);

        assert(s_context.screen == UI_SCREEN_WORKFLOW);
        assert(s_workflow_snapshot.step == WORKFLOW_STEP_OBJECT);
        assert((s_workflow_snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_START_MEASUREMENT)) != 0U);
        assert(s_acquisition_start_calls == 1U);
        assert(s_measurement_renders == 0U);
    }
}


static void test_home_measurements_without_active_object_opens_workflow_picker(void)
{
    reset_state();
    ui_screen_open_locked(UI_SCREEN_HOME);
    s_context.selected = 1U;
    ui_screen_handle_key_locked(UI_SCREEN_HOME, KEYPAD_KEY_OK);

    assert(s_context.screen == UI_SCREEN_OBJECTS);
    assert(s_context.objects_mode == UI_OBJECTS_SELECT_WORKFLOW);
    assert(s_object_list_requests == 1U);
    assert(s_acquisition_start_calls == 0U);

    s_context.objects_loaded = true;
    s_context.object_count = 1U;
    s_context.objects[0].active = true;
    (void)snprintf(s_context.objects[0].id, sizeof(s_context.objects[0].id), "%s", "picked-object");
    ui_handle_objects_key_locked(KEYPAD_KEY_OK);
    assert(s_context.screen == UI_SCREEN_WORKFLOW);
    assert(s_acquisition_start_calls == 1U);
}


static void test_objects_start_measurement_initializes_acquisition(void)
{
    reset_state();
    s_active_object_present = true;
    ui_screen_open_locked(UI_SCREEN_OBJECTS);

    s_context.objects_loaded = true;
    s_context.object_count = 1U;
    s_context.objects[0].active = true;
    (void)snprintf(s_context.objects[0].id, sizeof(s_context.objects[0].id), "%s", "active-object");
    ui_handle_objects_key_locked(KEYPAD_KEY_UP); /* wrap to Start measurement */
    assert(s_context.object_action == 3U);
    ui_handle_objects_key_locked(KEYPAD_KEY_OK);
    assert(s_context.objects_mode == UI_OBJECTS_SELECT_WORKFLOW);
    ui_handle_objects_key_locked(KEYPAD_KEY_OK);

    assert(s_context.screen == UI_SCREEN_WORKFLOW);
    assert(s_acquisition_start_calls == 1U);
    assert(s_measurement_renders == 0U);
}


static void test_workflow_initializes_before_readiness_and_retries_failures(void)
{
    for (uint8_t remotes = 0U; remotes <= 1U; ++remotes) {
        reset_state();
        s_active_object_present = true;
        s_workflow_snapshot.step = WORKFLOW_STEP_SENSOR_CHECK;
        s_workflow_snapshot.assigned_remotes = remotes;
        s_workflow_snapshot.allowed_actions = 0U;

        ui_screen_open_locked(UI_SCREEN_MEASUREMENTS);

        assert(s_context.screen == UI_SCREEN_WORKFLOW);
        assert(s_acquisition_start_calls == 1U);
        assert(s_workflow_snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
        assert(s_workflow_snapshot.allowed_actions == 0U);
    }

    reset_state();
    s_acquisition_start_result = ESP_ERR_INVALID_STATE;
    ui_screen_open_locked(UI_SCREEN_WORKFLOW);
    assert(s_context.screen == UI_SCREEN_WORKFLOW);
    assert(s_acquisition_start_calls == 1U);

    s_acquisition_start_result = ESP_OK;
    ui_screen_open_locked(UI_SCREEN_WORKFLOW);
    assert(s_context.screen == UI_SCREEN_WORKFLOW);
    assert(s_acquisition_start_calls == 2U);
}


static void test_recovery_events_update_home_state(void)
{
    reset_state();
    s_context.storage_recovery_failed = true;

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_STARTED,
        0
    );
    assert(s_context.storage_recovery_active);
    assert(!s_context.storage_recovery_failed);
    assert(s_home_renders == 1U);

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_FINISHED,
        ESP_OK
    );
    assert(!s_context.storage_recovery_active);
    assert(!s_context.storage_recovery_failed);
    assert(s_home_renders == 2U);
}


static void test_recovery_failure_is_sticky_until_next_start(void)
{
    reset_state();

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_FINISHED,
        -1
    );
    assert(!s_context.storage_recovery_active);
    assert(s_context.storage_recovery_failed);
    assert(s_home_renders == 1U);

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_STARTED,
        0
    );
    assert(s_context.storage_recovery_active);
    assert(!s_context.storage_recovery_failed);
    assert(s_home_renders == 2U);
}


static void test_recovery_event_does_not_force_other_screen_redraw(void)
{
    reset_state();
    s_context.screen = UI_SCREEN_SYSTEM;

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_STARTED,
        0
    );
    assert(s_context.storage_recovery_active);
    assert(s_home_renders == 0U);
}


static void test_recovery_start_refreshes_objects_screen_without_listing(void)
{
    reset_state();
    s_context.screen = UI_SCREEN_OBJECTS;

    ui_dispatch_app_event_locked(
        APP_EVENT_STORAGE_RECOVERY_STARTED,
        0
    );

    assert(s_context.storage_recovery_active);
    assert(s_objects_renders == 1U);
    assert(s_object_list_requests == 0U);
}


static void test_invalid_screen_is_noop(void)
{
    reset_state();
    s_context.selected = 3U;

    ui_screen_open_locked((ui_screen_t)UI_SCREEN_COUNT);
    assert(s_context.screen == UI_SCREEN_HOME);
    assert(s_context.selected == 3U);
}


#if defined(MAX449_CASE) && (MAX449_CASE < 1 || MAX449_CASE > 4)
#error "unknown MAX449_CASE for ui_home_navigation_test"
#endif
#ifndef MAX449_CASE
#define MAX449_CASE 0
#endif

int main(void)
{
    /* Keep every test referenced when compiling one selected case with -Werror. */
    switch (MAX449_CASE) {
        case 1:
            test_home_measurements_redirects_active_object_for_ok_and_right();
            test_workflow_initializes_before_readiness_and_retries_failures();
            return 0;
        case 2:
            test_home_measurements_without_active_object_opens_workflow_picker();
            return 0;
        case 3:
            test_measurements_route_decision_all_workflow_states();
            return 0;
        case 4:
            test_objects_start_measurement_initializes_acquisition();
            return 0;
        default:
            break;
    }
    test_home_registry_mapping();
    test_home_up_down_wrap_and_render();
    test_right_and_ok_open_selected_screen();
    test_wifi_entry_renders_the_wifi_screen();
    test_objects_entry_does_not_list_during_recovery();
    test_left_from_system_returns_home();
    test_dispatch_uses_current_screen_driver();
    test_measurements_route_decision_all_workflow_states();
    test_home_measurements_redirects_active_object_for_ok_and_right();
    test_workflow_initializes_before_readiness_and_retries_failures();
    test_home_measurements_without_active_object_opens_workflow_picker();
    test_objects_start_measurement_initializes_acquisition();
    test_recovery_events_update_home_state();
    test_recovery_failure_is_sticky_until_next_start();
    test_recovery_event_does_not_force_other_screen_redraw();
    test_recovery_start_refreshes_objects_screen_without_listing();
    test_invalid_screen_is_noop();

    puts("ui_home_navigation_test: PASS");
    return 0;
}
