#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "ui_internal.h"


static ui_context_t s_context;
static unsigned s_home_renders;
static unsigned s_objects_renders;
static unsigned s_measurement_renders;
static unsigned s_storage_renders;
static unsigned s_wifi_renders;
static unsigned s_system_renders;
static unsigned s_object_list_requests;
static unsigned s_storage_info_requests;


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


void ui_render_system_locked(void)
{
    ++s_system_renders;
}


void ui_measurements_refresh_locked(void)
{
}


void ui_system_refresh_locked(void)
{
}


void ui_handle_objects_key_locked(keypad_key_t key)
{
    (void)key;
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


esp_err_t storage_service_info_async(void)
{
    ++s_storage_info_requests;
    return ESP_OK;
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

    ui_screen_open_locked(UI_SCREEN_HOME);
    ui_dispatch_key_locked(KEYPAD_KEY_DOWN);
    assert(s_context.selected == 1U);

    ui_dispatch_key_locked(KEYPAD_KEY_OK);
    assert(s_context.screen == UI_SCREEN_MEASUREMENTS);
    assert(s_measurement_renders == 1U);
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


int main(void)
{
    test_home_registry_mapping();
    test_home_up_down_wrap_and_render();
    test_right_and_ok_open_selected_screen();
    test_wifi_entry_renders_the_wifi_screen();
    test_objects_entry_does_not_list_during_recovery();
    test_left_from_system_returns_home();
    test_dispatch_uses_current_screen_driver();
    test_recovery_events_update_home_state();
    test_recovery_failure_is_sticky_until_next_start();
    test_recovery_event_does_not_force_other_screen_redraw();
    test_recovery_start_refreshes_objects_screen_without_listing();
    test_invalid_screen_is_noop();

    puts("ui_home_navigation_test: PASS");
    return 0;
}
