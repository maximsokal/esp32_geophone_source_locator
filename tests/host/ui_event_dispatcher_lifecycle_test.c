#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "app_events.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "runtime_observability.h"
#include "ui_internal.h"
#include "ui_i18n_nvs.h"

ESP_EVENT_DECLARE_BASE(UI_I18N_STORE_EVENT);

static esp_event_loop_handle_t fake_loop;
static esp_err_t fake_register_result;
static unsigned register_failure_call;
static esp_event_handler_t captured_handler;
static void *captured_handler_arg;
static unsigned register_calls;
static unsigned unregister_calls;
static unsigned wake_calls;
static unsigned delay_calls;
static unsigned key_dispatch_calls;
static unsigned app_dispatch_calls;
static unsigned beat_calls;
static keypad_key_t last_key;
static int32_t last_app_event_id;
static int32_t last_app_event_value;
static int64_t fake_time_us;
static ui_context_t fake_context;

static bool wake_block;
static bool wake_entered;
static bool wake_release;
static bool stop_returned;
static bool app_event_registered;
static bool locale_event_registered;

bool ui_i18n_nvs_save_locale(ui_locale_t locale)
{
    (void)locale;
    return true;
}


void ui_render_system_locked(void)
{
}

#ifdef _WIN32
static CRITICAL_SECTION sync_lock;
static CONDITION_VARIABLE sync_cond;
typedef HANDLE test_thread_t;

typedef DWORD (WINAPI *test_thread_fn_t)(LPVOID);

static void sync_init(void)
{
    InitializeCriticalSection(&sync_lock);
    InitializeConditionVariable(&sync_cond);
}

static void sync_destroy(void)
{
    DeleteCriticalSection(&sync_lock);
}

static void sync_enter(void)
{
    EnterCriticalSection(&sync_lock);
}

static void sync_leave(void)
{
    LeaveCriticalSection(&sync_lock);
}

static void sync_wait(void)
{
    SleepConditionVariableCS(&sync_cond, &sync_lock, INFINITE);
}

static void sync_broadcast(void)
{
    WakeAllConditionVariable(&sync_cond);
}

static test_thread_t test_thread_start(test_thread_fn_t fn, void *arg)
{
    HANDLE thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    assert(thread != NULL);
    return thread;
}

static void test_thread_join(test_thread_t thread)
{
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
}

static void test_sleep_ms(unsigned ms)
{
    Sleep(ms);
}
#else
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
typedef pthread_t test_thread_t;
typedef void *(*test_thread_fn_t)(void *);

static void sync_init(void)
{
}

static void sync_destroy(void)
{
    assert(pthread_cond_destroy(&sync_cond) == 0);
    assert(pthread_mutex_destroy(&sync_lock) == 0);
}

static void sync_enter(void)
{
    assert(pthread_mutex_lock(&sync_lock) == 0);
}

static void sync_leave(void)
{
    assert(pthread_mutex_unlock(&sync_lock) == 0);
}

static void sync_wait(void)
{
    assert(pthread_cond_wait(&sync_cond, &sync_lock) == 0);
}

static void sync_broadcast(void)
{
    assert(pthread_cond_broadcast(&sync_cond) == 0);
}

static test_thread_t test_thread_start(test_thread_fn_t fn, void *arg)
{
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fn, arg) == 0);
    return thread;
}

static void test_thread_join(test_thread_t thread)
{
    assert(pthread_join(thread, NULL) == 0);
}

static void test_sleep_ms(unsigned ms)
{
    struct timespec duration = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    (void)nanosleep(&duration, NULL);
}
#endif

static void reset_observation(void)
{
    register_calls = 0U;
    unregister_calls = 0U;
    register_failure_call = 0U;
    captured_handler = NULL;
    captured_handler_arg = NULL;
    app_event_registered = false;
    locale_event_registered = false;
    wake_calls = 0U;
    delay_calls = 0U;
    key_dispatch_calls = 0U;
    app_dispatch_calls = 0U;
    beat_calls = 0U;
    last_key = KEYPAD_KEY_NONE;
    last_app_event_id = 0;
    last_app_event_value = 0;
    fake_time_us = 1000;
    fake_context.screen = UI_SCREEN_HOME;
    fake_context.selected = 0U;

    sync_enter();
    wake_block = false;
    wake_entered = false;
    wake_release = false;
    stop_returned = false;
    sync_leave();
}

esp_event_loop_handle_t app_events_get_loop(void)
{
    return fake_loop;
}

esp_err_t esp_event_handler_register_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler,
    void *event_handler_arg
)
{
    assert(loop == fake_loop);
    assert(event_id == ESP_EVENT_ANY_ID);
    ++register_calls;
    captured_handler = event_handler;
    captured_handler_arg = event_handler_arg;
    if (register_calls == register_failure_call) {
        return fake_register_result;
    }
    if (event_base == APP_EVENT) {
        assert(!app_event_registered);
        app_event_registered = true;
    }
    else {
        assert(event_base == UI_I18N_STORE_EVENT);
        assert(!locale_event_registered);
        locale_event_registered = true;
    }
    return ESP_OK;
}

esp_err_t esp_event_handler_unregister_with(
    esp_event_loop_handle_t loop,
    esp_event_base_t event_base,
    int32_t event_id,
    esp_event_handler_t event_handler
)
{
    assert(loop == fake_loop);
    assert(event_id == ESP_EVENT_ANY_ID);
    assert(event_handler == captured_handler);
    if (event_base == APP_EVENT) {
        assert(app_event_registered);
        app_event_registered = false;
    }
    else {
        assert(event_base == UI_I18N_STORE_EVENT);
        assert(locale_event_registered);
        locale_event_registered = false;
    }
    ++unregister_calls;
    return ESP_OK;
}

esp_err_t lvgl_port_task_wake(int event, void *data)
{
    assert(event == LVGL_PORT_EVENT_USER);
    assert(data == NULL);

    sync_enter();
    ++wake_calls;
    if (wake_block) {
        wake_entered = true;
        sync_broadcast();
        while (!wake_release) {
            sync_wait();
        }
    }
    sync_leave();
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    fake_time_us += 1000;
    return fake_time_us;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    sync_enter();
    ++delay_calls;
    sync_broadcast();
    sync_leave();
    test_sleep_ms(1U);
}

void runtime_observability_beat(int task, uint64_t progress)
{
    assert(task == RUNTIME_TASK_UI);
    (void)progress;
    ++beat_calls;
}

ui_context_t *ui_context(void)
{
    return &fake_context;
}

void ui_dispatch_key_locked(keypad_key_t key)
{
    ++key_dispatch_calls;
    last_key = key;
    fake_context.selected++;
}

void ui_dispatch_app_event_locked(int event_type, int32_t event_value)
{
    ++app_dispatch_calls;
    last_app_event_id = event_type;
    last_app_event_value = event_value;
}

static void emit_event(int32_t event_id, int32_t value)
{
    assert(captured_handler != NULL);
    captured_handler(captured_handler_arg, APP_EVENT, event_id, &value);
}

#ifdef _WIN32
static DWORD WINAPI handler_thread_main(LPVOID argument)
{
    const int32_t value = *(const int32_t *)argument;
    emit_event(77, value);
    return 0;
}

static DWORD WINAPI stop_thread_main(LPVOID argument)
{
    (void)argument;
    ui_event_dispatcher_stop();
    sync_enter();
    stop_returned = true;
    sync_broadcast();
    sync_leave();
    return 0;
}
#else
static void *handler_thread_main(void *argument)
{
    const int32_t value = *(const int32_t *)argument;
    emit_event(77, value);
    return NULL;
}

static void *stop_thread_main(void *argument)
{
    (void)argument;
    ui_event_dispatcher_stop();
    sync_enter();
    stop_returned = true;
    sync_broadcast();
    sync_leave();
    return NULL;
}
#endif

static void wait_until_wake_entered(void)
{
    sync_enter();
    while (!wake_entered) {
        sync_wait();
    }
    sync_leave();
}

static void wait_until_stop_is_waiting(void)
{
    sync_enter();
    while (delay_calls == 0U) {
        sync_wait();
    }
    assert(!stop_returned);
    sync_leave();
}

int main(void)
{
    sync_init();
    reset_observation();

    fake_loop = NULL;
    fake_register_result = ESP_OK;
    captured_handler = NULL;
    captured_handler_arg = NULL;
    assert(ui_event_dispatcher_start() == ESP_ERR_INVALID_STATE);
    assert(register_calls == 0U);

    fake_loop = (void *)(uintptr_t)1U;
    fake_register_result = ESP_FAIL;
    register_failure_call = 1U;
    assert(ui_event_dispatcher_start() == ESP_FAIL);
    assert(register_calls == 1U);
    assert(unregister_calls == 0U);
    assert(captured_handler != NULL);
    emit_event(APP_EVENT_KEYPAD, KEYPAD_KEY_UP);
    assert(wake_calls == 0U);
    ui_event_dispatcher_dispatch_pending_locked();
    assert(key_dispatch_calls == 0U);

    reset_observation();
    fake_loop = (void *)(uintptr_t)1U;
    fake_register_result = ESP_FAIL;
    register_failure_call = 2U;
    assert(ui_event_dispatcher_start() == ESP_FAIL);
    assert(register_calls == 2U);
    assert(unregister_calls == 1U);
    assert(!app_event_registered && !locale_event_registered);
    emit_event(APP_EVENT_KEYPAD, KEYPAD_KEY_UP);
    assert(wake_calls == 0U);

    reset_observation();
    fake_loop = (void *)(uintptr_t)1U;
    fake_register_result = ESP_OK;
    register_failure_call = 0U;
    assert(ui_event_dispatcher_start() == ESP_OK);
    assert(register_calls == 2U);
    assert(ui_event_dispatcher_start() == ESP_OK);
    assert(register_calls == 2U);

    emit_event(APP_EVENT_KEYPAD, KEYPAD_KEY_UP);
    assert(wake_calls == 1U);
    ui_event_dispatcher_dispatch_pending_locked();
    assert(key_dispatch_calls == 1U);
    assert(last_key == KEYPAD_KEY_UP);
    assert(beat_calls == 1U);

    emit_event(55, 1234);
    assert(wake_calls == 2U);
    ui_event_dispatcher_stop();
    assert(unregister_calls == 2U);
    assert(!app_event_registered && !locale_event_registered);
    ui_event_dispatcher_dispatch_pending_locked();
    assert(app_dispatch_calls == 0U);
    assert(last_app_event_id == 0 && last_app_event_value == 0);

    emit_event(55, 4321);
    assert(wake_calls == 2U);
    assert(app_dispatch_calls == 0U);
    assert(last_app_event_id == 0 && last_app_event_value == 0);

    ui_event_dispatcher_stop();
    assert(unregister_calls == 2U);

    reset_observation();
    fake_loop = (void *)(uintptr_t)1U;
    fake_register_result = ESP_OK;
    assert(ui_event_dispatcher_start() == ESP_OK);

    sync_enter();
    wake_block = true;
    sync_leave();

    const int32_t threaded_value = 9876;
    test_thread_t handler_thread =
        test_thread_start(handler_thread_main, (void *)&threaded_value);
    wait_until_wake_entered();

    test_thread_t stop_thread =
        test_thread_start(stop_thread_main, NULL);
    wait_until_stop_is_waiting();

    sync_enter();
    assert(!stop_returned);
    wake_release = true;
    sync_broadcast();
    sync_leave();

    test_thread_join(handler_thread);
    test_thread_join(stop_thread);

    sync_enter();
    assert(stop_returned);
    sync_leave();
    assert(delay_calls > 0U);
    assert(unregister_calls == 2U);
    assert(wake_calls == 1U);

    emit_event(77, 1111);
    assert(wake_calls == 1U);

    ui_event_dispatcher_dispatch_pending_locked();
    assert(app_dispatch_calls == 0U);
    assert(last_app_event_id == 0 && last_app_event_value == 0);

    sync_destroy();
    puts("ui event dispatcher lifecycle tests passed");
    return 0;
}
