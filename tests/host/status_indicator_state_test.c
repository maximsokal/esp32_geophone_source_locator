#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "status_indicator.h"
#include "app_events.h"
#include "esp_event.h"
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
#include "led_indicator_gpio.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "hw_inventory.h"
#endif

const char *APP_EVENT = "APP_EVENT";
char host_esp_log_last[256];

static int s_loop_token;
static esp_event_loop_handle_t s_loop;
static esp_err_t s_register_result = ESP_OK;
static unsigned s_register_calls;
static unsigned s_unregister_calls;
static esp_event_handler_t s_handler;
static app_event_type_t s_posted_event;
static int32_t s_posted_value;
static unsigned s_post_calls;
static esp_err_t s_post_result = ESP_OK;

#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
static struct {
    bool allocated;
    bool on;
} s_leds[3];
static unsigned s_create_calls;
static unsigned s_set_calls;
static unsigned s_delete_calls;
static unsigned s_delay_calls;
static unsigned s_fail_create_at;
static unsigned s_fail_set_at;
static int s_timer_token;
static bool s_timer_created;
static bool s_fail_timer_create;
static bool s_fail_timer_start;
static TimerCallbackFunction_t s_health_tick;
static hw_module_info_t s_modules[HW_MODULE_COUNT];

size_t hw_inventory_snapshot(hw_module_info_t *out, size_t cap)
{
    assert(out != NULL && cap == HW_MODULE_COUNT);
    memcpy(out, s_modules, sizeof(s_modules));
    return HW_MODULE_COUNT;
}

TimerHandle_t xTimerCreate(const char *name, TickType_t period, BaseType_t reload,
    void *id, TimerCallbackFunction_t callback)
{
    assert(strcmp(name, "status_health") == 0);
    assert(period == pdMS_TO_TICKS(1000) && reload == pdTRUE && id == NULL);
    assert(!s_timer_created);
    if (s_fail_timer_create) {
        return NULL;
    }
    s_timer_created = true;
    s_health_tick = callback;
    return &s_timer_token;
}

BaseType_t xTimerStart(TimerHandle_t timer, TickType_t wait)
{
    assert(timer == &s_timer_token && wait == 0 && s_timer_created);
    return s_fail_timer_start ? 0 : pdPASS;
}

BaseType_t xTimerDelete(TimerHandle_t timer, TickType_t wait)
{
    assert(timer == &s_timer_token && wait == portMAX_DELAY && s_timer_created);
    s_timer_created = false;
    return pdPASS;
}

#if !defined(STATUS_INDICATOR_FAILURE_TEST)
static void health_tick(void)
{
    const unsigned calls = s_set_calls;
    s_health_tick(&s_timer_token);
    assert(s_set_calls == calls);
}
#endif

esp_err_t led_indicator_new_gpio_device(
    const led_indicator_config_t *config,
    const led_indicator_gpio_config_t *gpio,
    led_indicator_handle_t *handle)
{
    const int pins[] = {6, 5, 4};
    unsigned index;
    assert(config != NULL && gpio != NULL && handle != NULL);
    ++s_create_calls;
    if (s_create_calls == s_fail_create_at) {
        return ESP_FAIL;
    }
    for (index = 0U; index < 3U; ++index) {
        if (pins[index] == gpio->gpio_num) {
            break;
        }
    }
    assert(index < 3U && !s_leds[index].allocated);
    assert(gpio->is_active_level_high);
    s_leds[index].allocated = true;
    s_leds[index].on = false;
    *handle = &s_leds[index];
    return ESP_OK;
}

esp_err_t led_indicator_set_on_off(led_indicator_handle_t handle, bool on)
{
    ++s_set_calls;
    if (s_set_calls == s_fail_set_at) {
        return ESP_FAIL;
    }
    for (unsigned index = 0U; index < 3U; ++index) {
        if (handle == &s_leds[index]) {
            assert(s_leds[index].allocated);
            s_leds[index].on = on;
            return ESP_OK;
        }
    }
    assert(false);
    return ESP_FAIL;
}

esp_err_t led_indicator_delete(led_indicator_handle_t handle)
{
    for (unsigned index = 0U; index < 3U; ++index) {
        if (handle == &s_leds[index]) {
            assert(s_leds[index].allocated && !s_leds[index].on);
            s_leds[index].allocated = false;
            ++s_delete_calls;
            return ESP_OK;
        }
    }
    assert(false);
    return ESP_FAIL;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(75));
    assert(s_leds[s_delay_calls % 3U].on);
    for (unsigned index = 0U; index < 3U; ++index) {
        assert(s_leds[index].on == (index == s_delay_calls % 3U));
    }
    ++s_delay_calls;
}

static void assert_leds(bool green, bool yellow, bool red)
{
    assert(s_leds[0].on == green);
    assert(s_leds[1].on == yellow);
    assert(s_leds[2].on == red);
}
#endif

esp_event_loop_handle_t app_events_get_loop(void)
{
    return s_loop;
}

esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    s_posted_event = type;
    s_posted_value = value;
    ++s_post_calls;
    return s_post_result;
}

esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value)
{
    return app_events_post(type, value);
}

esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler, void *arg)
{
    (void)arg;
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == ESP_EVENT_ANY_ID);
    ++s_register_calls;
    if (s_register_result == ESP_OK) {
        s_handler = handler;
    }
    return s_register_result;
}

esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id, esp_event_handler_t handler)
{
    assert(loop == &s_loop_token);
    assert(base == APP_EVENT);
    assert(id == ESP_EVENT_ANY_ID);
    assert(handler == s_handler);
    ++s_unregister_calls;
    return ESP_OK;
}

static void emit(app_event_type_t event)
{
    assert(s_handler != NULL);
    s_handler(NULL, APP_EVENT, event, NULL);
}

int main(void)
{
    /* Some variants only write these probes; reading them keeps -Werror quiet. */
    (void)s_post_calls;
    (void)s_posted_event;
    (void)s_posted_value;

    assert(status_indicator_get_state() == STATUS_INDICATOR_NORMAL);
    assert(!status_indicator_backend_enabled());

    s_loop = NULL;
    assert(status_indicator_init() == ESP_ERR_INVALID_STATE);

    s_loop = &s_loop_token;
    s_register_result = ESP_FAIL;
    assert(status_indicator_init() == ESP_FAIL);
    assert(s_register_calls == 1U);

#if defined(STATUS_INDICATOR_FAILURE_TEST)
    s_register_result = ESP_OK;
    for (unsigned step = 1U; step <= 3U; ++step) {
        s_fail_create_at = s_create_calls + step;
        const unsigned deleted = s_delete_calls;
        assert(status_indicator_init() == ESP_FAIL);
        assert(!status_indicator_backend_enabled());
        assert(s_delete_calls == deleted + step - 1U);
        assert_leds(false, false, false);
    }
    s_fail_create_at = 0U;
    s_delay_calls = 0U;
    s_fail_set_at = s_set_calls + 1U;
    assert(status_indicator_init() == ESP_FAIL);
    assert(!status_indicator_backend_enabled());
    assert_leds(false, false, false);
    s_fail_set_at = 0U;
    s_fail_timer_create = true;
    assert(status_indicator_init() == ESP_FAIL);
    assert(!status_indicator_backend_enabled() && !s_timer_created);
    s_fail_timer_create = false;
    s_fail_timer_start = true;
    assert(status_indicator_init() == ESP_FAIL);
    assert(!status_indicator_backend_enabled() && !s_timer_created);
    s_fail_timer_start = false;
    assert(status_indicator_init() == ESP_OK);
    s_fail_set_at = s_set_calls + 1U;
    emit(APP_EVENT_STATUS_WARNING);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);
    assert(!status_indicator_backend_enabled());
    assert_leds(false, false, false);
    status_indicator_stop();
    assert(status_indicator_init() == ESP_OK);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);
    assert_leds(false, true, false);
    status_indicator_stop();
    return 0;
#else

    s_register_result = ESP_OK;
    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 2U);
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    assert(status_indicator_backend_enabled());
    assert(s_create_calls == 3U && s_delay_calls == 3U);
    assert_leds(true, false, false);
    for (unsigned id = 0U; id < (unsigned)HW_MODULE_COUNT; ++id) {
        s_modules[id].id = (hw_module_id_t)id;
    }
    health_tick();
    s_modules[HW_MODULE_BAROMETER].state = HW_STATE_ABSENT;
    s_modules[HW_MODULE_AD7606].requirement = HW_REQ_REQUIRED;
    health_tick();
    assert(s_post_calls == 0U);
    s_modules[HW_MODULE_BAROMETER].state = HW_STATE_DEGRADED;
    health_tick();
    assert(s_post_calls == 1U && s_posted_event == APP_EVENT_STATUS_WARNING);
#else
    assert(!status_indicator_backend_enabled());
#endif
    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 2U);

    assert(status_indicator_post(APP_STATUS_WARNING) == ESP_OK);
    assert(s_posted_event == APP_EVENT_STATUS_WARNING && s_posted_value == 0);
    emit(APP_EVENT_STATUS_WARNING);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    assert_leds(false, true, false);
#endif

    assert(status_indicator_post(APP_STATUS_NORMAL) == ESP_OK);
    assert(s_posted_event == APP_EVENT_STATUS_CLEAR_NON_STICKY);
    emit(APP_EVENT_STATUS_CLEAR_NON_STICKY);
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);

#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    s_modules[HW_MODULE_BAROMETER].state = HW_STATE_FAULT;
    strcpy(s_modules[HW_MODULE_BAROMETER].reason, "bus stuck");
    s_post_result = ESP_FAIL;
    host_esp_log_last[0] = '\0';
    health_tick();
    assert(status_indicator_get_state() == STATUS_INDICATOR_WARNING);
    assert(host_esp_log_last[0] == '\0'); /* a failed post is retried silently */
    const unsigned failed_posts = s_post_calls;
    s_post_result = ESP_OK;
    health_tick();
    assert(s_post_calls == failed_posts + 1U && s_posted_event == APP_EVENT_STATUS_ERROR);
    {
        /* The transition to ERROR must name the module that caused it. */
        char expected[160];
        (void)snprintf(expected, sizeof(expected),
            "STATUS_HEALTH event=%d module_id=%d state=%d requirement=%d reason=bus stuck",
            (int)APP_EVENT_STATUS_ERROR, (int)HW_MODULE_BAROMETER,
            (int)HW_STATE_FAULT, (int)HW_REQ_OPTIONAL);
        assert(strcmp(host_esp_log_last, expected) == 0);
    }
    health_tick();
    assert(s_post_calls == failed_posts + 1U);
    emit(s_posted_event);
    s_modules[HW_MODULE_BAROMETER].state = HW_STATE_PRESENT;
    health_tick();
    assert(s_posted_event == APP_EVENT_STATUS_CLEAR_NON_STICKY);
    emit(s_posted_event);
    s_modules[HW_MODULE_AD7606].state = HW_STATE_ABSENT;
    health_tick();
    assert(s_posted_event == APP_EVENT_STATUS_ERROR);
#else
    emit(APP_EVENT_STATUS_ERROR);
#endif
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);
    emit(APP_EVENT_STATUS_WARNING);
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    assert_leds(false, false, true);
#endif

    assert(status_indicator_post((app_status_event_t)99) == ESP_ERR_INVALID_ARG);

    status_indicator_stop();
    assert(s_unregister_calls == 1U);
    assert(!status_indicator_backend_enabled());
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    assert(s_delete_calls == 3U);
    assert_leds(false, false, false);
#endif
    status_indicator_stop();
    assert(s_unregister_calls == 1U);

    assert(status_indicator_init() == ESP_OK);
    assert(s_register_calls == 3U);
    assert(status_indicator_get_state() == STATUS_INDICATOR_ERROR);
#if defined(CONFIG_APP_STATUS_INDICATOR_ENABLED) && CONFIG_APP_STATUS_INDICATOR_ENABLED
    assert_leds(false, false, true);
#endif

    return 0;
#endif
}
