#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_lvgl_port.h"
#include "freertos/task.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "ui_internal.h"

typedef enum {
    EVENT_GATE_BEGIN = 1,
    EVENT_LVGL_UNLOCK,
    EVENT_STORAGE_READY,
    EVENT_LVGL_LOCK,
    EVENT_DELAY,
    EVENT_GATE_END,
    EVENT_NOTICE,
    EVENT_RENDER,
    EVENT_ERASE,
} event_t;

static ui_context_t s_context;
static event_t s_events[16];
static size_t s_event_count;
static bool s_lvgl_locked;
static bool s_gate_open;
static bool s_storage_ready;
static unsigned s_erase_calls;
static unsigned s_failed_lock_attempts;

static void record(event_t event)
{
    assert(s_event_count < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_event_count++] = event;
}

static void reset(bool storage_ready, bool gate_open)
{
    s_context = (ui_context_t){
        .screen = UI_SCREEN_STORAGE,
        .storage_action = 1U,
        .storage_erase_confirm = false,
    };
    s_event_count = 0U;
    s_lvgl_locked = true;
    s_gate_open = gate_open;
    s_storage_ready = storage_ready;
    s_erase_calls = 0U;
    s_failed_lock_attempts = 0U;
}

ui_context_t *ui_context(void)
{
    return &s_context;
}

bool ui_storage_query_begin(void)
{
    assert(s_lvgl_locked);
    record(EVENT_GATE_BEGIN);
    return s_gate_open;
}

void ui_storage_query_end(void)
{
    assert(s_lvgl_locked);
    record(EVENT_GATE_END);
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    assert(!s_lvgl_locked);
    assert(timeout_ms == 0U);
    record(EVENT_LVGL_LOCK);
    if (s_failed_lock_attempts > 0U) {
        --s_failed_lock_attempts;
        return false;
    }
    s_lvgl_locked = true;
    return true;
}

void lvgl_port_unlock(void)
{
    assert(s_lvgl_locked);
    record(EVENT_LVGL_UNLOCK);
    s_lvgl_locked = false;
}

void vTaskDelay(uint32_t ticks)
{
    assert(!s_lvgl_locked);
    assert(ticks == 1U);
    record(EVENT_DELAY);
}

bool storage_manager_is_ready(void)
{
    assert(!s_lvgl_locked);
    record(EVENT_STORAGE_READY);
    return s_storage_ready;
}

bool storage_service_is_busy(void)
{
    return false;
}

esp_err_t storage_service_info_async(void)
{
    return ESP_OK;
}

esp_err_t storage_service_erase_data_async(void)
{
    ++s_erase_calls;
    record(EVENT_ERASE);
    return ESP_OK;
}

void ui_open_home_locked(void)
{
    s_context.screen = UI_SCREEN_HOME;
}

void ui_move_index(size_t *value, size_t count, bool down)
{
    (void)value;
    (void)count;
    (void)down;
}

void ui_set_notice_locked(const char *text)
{
    (void)text;
    record(EVENT_NOTICE);
}

void ui_render_storage_locked(void)
{
    record(EVENT_RENDER);
}

const char *ui_tr(ui_i18n_key_t key)
{
    (void)key;
    return "test";
}

static void assert_events(const event_t *expected, size_t count)
{
    assert(s_event_count == count);
    for (size_t i = 0U; i < count; ++i) {
        assert(s_events[i] == expected[i]);
    }
}

static void test_readiness_query_runs_outside_lvgl_and_relocks(void)
{
    const event_t expected[] = {
        EVENT_GATE_BEGIN,
        EVENT_LVGL_UNLOCK,
        EVENT_STORAGE_READY,
        EVENT_LVGL_LOCK,
        EVENT_DELAY,
        EVENT_LVGL_LOCK,
        EVENT_DELAY,
        EVENT_LVGL_LOCK,
        EVENT_GATE_END,
        EVENT_NOTICE,
        EVENT_RENDER,
    };
    reset(true, true);
    s_failed_lock_attempts = 2U;
    ui_handle_storage_key_locked(KEYPAD_KEY_OK);
    assert(s_lvgl_locked);
    assert(s_context.storage_erase_confirm);
    assert(s_erase_calls == 0U);
    assert_events(expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_unready_storage_clears_confirmation_without_erasing(void)
{
    const event_t expected[] = {
        EVENT_GATE_BEGIN,
        EVENT_LVGL_UNLOCK,
        EVENT_STORAGE_READY,
        EVENT_LVGL_LOCK,
        EVENT_GATE_END,
    };
    reset(false, true);
    s_context.storage_erase_confirm = true;
    ui_handle_storage_key_locked(KEYPAD_KEY_OK);
    assert(s_lvgl_locked);
    assert(!s_context.storage_erase_confirm);
    assert(s_erase_calls == 0U);
    assert_events(expected, sizeof(expected) / sizeof(expected[0]));
}

static void test_closed_query_gate_returns_with_lvgl_lock_held(void)
{
    const event_t expected[] = {EVENT_GATE_BEGIN};
    reset(true, false);
    ui_handle_storage_key_locked(KEYPAD_KEY_OK);
    assert(s_lvgl_locked);
    assert(!s_context.storage_erase_confirm);
    assert(s_erase_calls == 0U);
    assert_events(expected, sizeof(expected) / sizeof(expected[0]));
}

int main(void)
{
    test_readiness_query_runs_outside_lvgl_and_relocks();
    test_unready_storage_clears_confirmation_without_erasing();
    test_closed_query_gate_returns_with_lvgl_lock_held();
    return 0;
}
