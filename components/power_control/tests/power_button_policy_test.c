#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "button_gpio.h"
#include "driver/gpio.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "app_events.h"
#include "iot_button.h"


typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);

esp_err_t power_button_adapter_init(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
);
esp_err_t power_button_adapter_arm_after_startup(void);
esp_err_t power_button_adapter_set_notification_hook(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
);
void power_button_adapter_wait_for_shutdown_request(void);
bool power_button_adapter_is_shutdown_requested(void);
void power_button_adapter_stop(void);


static EventBits_t fake_event_bits;
static int fake_gpio_level;
static button_event_t fake_event;
static button_cb_t callbacks[BUTTON_EVENT_MAX];
static void *callback_contexts[BUTTON_EVENT_MAX];
static button_gpio_config_t captured_gpio_config;
static button_config_t captured_button_config;
static unsigned hook_calls;
static unsigned hook_bits_at_call;
static unsigned app_event_posts;
static esp_err_t fake_best_effort_result;
static uint32_t fake_timestamp_ms;
static unsigned delay_calls;
static TickType_t delayed_ticks;
static bool keypad_delivery_enabled;
static unsigned keypad_gate_close_calls;
static EventBits_t keypad_gate_close_bits;


static uint32_t fake_button_time_to_ticks(uint32_t time_ms)
{
    const uint32_t ticks = time_ms / CONFIG_BUTTON_PERIOD_TIME_MS;
    return ticks > 0U ? ticks : 1U;
}


static void reset_fakes(void)
{
    fake_event_bits = 0U;
    fake_gpio_level = 0;
    fake_event = BUTTON_NONE_PRESS;
    for (size_t i = 0; i < BUTTON_EVENT_MAX; ++i) {
        callbacks[i] = NULL;
        callback_contexts[i] = NULL;
    }
    hook_calls = 0U;
    hook_bits_at_call = 0U;
    app_event_posts = 0U;
    fake_best_effort_result = ESP_OK;
    fake_timestamp_ms = 0U;
    delay_calls = 0U;
    delayed_ticks = 0U;
    keypad_delivery_enabled = true;
    keypad_gate_close_calls = 0U;
    keypad_gate_close_bits = 0U;
}


uint32_t esp_log_timestamp(void)
{
    return fake_timestamp_ms;
}


void vTaskDelay(TickType_t ticks)
{
    delay_calls++;
    delayed_ticks = ticks;
}


void app_events_set_keypad_delivery_enabled(bool enabled)
{
    keypad_delivery_enabled = enabled;
    if (!enabled) {
        keypad_gate_close_calls++;
        keypad_gate_close_bits = fake_event_bits;
    }
}


bool app_events_keypad_delivery_enabled(void)
{
    return keypad_delivery_enabled;
}


esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    assert(type == APP_EVENT_POWER_SHUTDOWN_REQUESTED);
    assert(value == 0);
    app_event_posts++;
    return ESP_OK;
}


esp_err_t app_events_post_best_effort(app_event_type_t type, int32_t value)
{
    assert(type == APP_EVENT_POWER_SHUTDOWN_REQUESTED);
    assert(value == 0);
    app_event_posts++;
    return fake_best_effort_result;
}


EventGroupHandle_t xEventGroupCreate(void)
{
    return &fake_event_bits;
}


void vEventGroupDelete(EventGroupHandle_t event_group)
{
    (void)event_group;
}


EventBits_t xEventGroupSetBits(EventGroupHandle_t event_group, EventBits_t bits)
{
    (void)event_group;
    fake_event_bits |= bits;
    return fake_event_bits;
}


EventBits_t xEventGroupClearBits(EventGroupHandle_t event_group, EventBits_t bits)
{
    (void)event_group;
    const EventBits_t previous = fake_event_bits;
    fake_event_bits &= ~bits;
    return previous;
}


EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t event_group,
    EventBits_t bits_to_wait_for,
    BaseType_t clear_on_exit,
    BaseType_t wait_for_all_bits,
    TickType_t ticks_to_wait
)
{
    (void)event_group;
    (void)clear_on_exit;
    (void)wait_for_all_bits;
    (void)ticks_to_wait;
    return fake_event_bits & bits_to_wait_for;
}


EventBits_t xEventGroupGetBits(EventGroupHandle_t event_group)
{
    (void)event_group;
    return fake_event_bits;
}


int gpio_get_level(gpio_num_t gpio_num)
{
    (void)gpio_num;
    return fake_gpio_level;
}


esp_err_t iot_button_new_gpio_device(
    const button_config_t *button_config,
    const void *gpio_config,
    button_handle_t *ret_button
)
{
    captured_button_config = *button_config;
    captured_gpio_config = *(const button_gpio_config_t *)gpio_config;
    *ret_button = &captured_gpio_config;
    return ESP_OK;
}


esp_err_t iot_button_register_cb(
    button_handle_t button,
    button_event_t event,
    void *event_args,
    button_cb_t callback,
    void *user_data
)
{
    (void)button;
    (void)event_args;

    /* Mirror the espressif/button 4.2.0 registration contract that caused the
     * real boot loop: the effective long-press tick must be strictly greater
     * than short_press_ticks for BUTTON_LONG_PRESS_START. */
    if (event == BUTTON_LONG_PRESS_START &&
        fake_button_time_to_ticks(captured_button_config.long_press_time) <=
            fake_button_time_to_ticks(captured_button_config.short_press_time)) {
        return ESP_ERR_INVALID_ARG;
    }

    callbacks[event] = callback;
    callback_contexts[event] = user_data;
    return ESP_OK;
}


button_event_t iot_button_get_event(button_handle_t button)
{
    (void)button;
    return fake_event;
}


uint8_t iot_button_get_key_level(button_handle_t button)
{
    (void)button;
    return fake_gpio_level != 0 ? 1U : 0U;
}


esp_err_t iot_button_delete(button_handle_t button)
{
    (void)button;
    return ESP_OK;
}


static void invoke(button_event_t event)
{
    assert(callbacks[event] != NULL);
    fake_event = event;
    callbacks[event](&captured_gpio_config, callback_contexts[event]);
}


static void notification_hook(void *user_context)
{
    (void)user_context;
    hook_calls++;
    hook_bits_at_call = fake_event_bits;
}


static void noop_button_callback(void *button_handle, void *user_data)
{
    (void)button_handle;
    (void)user_data;
}


static void test_button_registration_rejects_equal_effective_ticks(void)
{
    reset_fakes();

    const button_gpio_config_t gpio_config = {
        .gpio_num = 21,
        .active_level = 1U,
        .enable_power_save = false,
        .disable_pull = true,
    };
    button_handle_t button = NULL;

    const button_config_t invalid_config = {
        .short_press_time = 150U,
        .long_press_time = 150U,
    };
    assert(iot_button_new_gpio_device(
        &invalid_config,
        &gpio_config,
        &button
    ) == ESP_OK);
    assert(iot_button_register_cb(
        button,
        BUTTON_LONG_PRESS_START,
        NULL,
        noop_button_callback,
        NULL
    ) == ESP_ERR_INVALID_ARG);

    const button_config_t valid_config = {
        .short_press_time = 50U,
        .long_press_time = 150U,
    };
    assert(iot_button_new_gpio_device(
        &valid_config,
        &gpio_config,
        &button
    ) == ESP_OK);
    assert(fake_button_time_to_ticks(valid_config.long_press_time) >
        fake_button_time_to_ticks(valid_config.short_press_time));
    assert(iot_button_register_cb(
        button,
        BUTTON_LONG_PRESS_START,
        NULL,
        noop_button_callback,
        NULL
    ) == ESP_OK);
}


static void test_boot_long_press_cannot_latch_before_explicit_arm(void)
{
    reset_fakes();
    fake_gpio_level = 1;
    assert(power_button_adapter_init(notification_hook, NULL) == ESP_OK);
    assert(captured_gpio_config.gpio_num == 21);
    assert(captured_gpio_config.active_level == 1U);
    assert(captured_gpio_config.disable_pull);
    assert(captured_button_config.short_press_time == 50U);
    assert(captured_button_config.long_press_time == 150U);

    fake_timestamp_ms = 1000U;
    invoke(BUTTON_PRESS_DOWN);
    fake_timestamp_ms = 1150U;
    invoke(BUTTON_LONG_PRESS_START);
    assert(!power_button_adapter_is_shutdown_requested());
    assert(keypad_delivery_enabled);
    assert(keypad_gate_close_calls == 0U);
    assert(hook_calls == 0U);
    assert(app_event_posts == 0U);

    /* Even a debounced release during the long boot does not arm shutdown. */
    fake_gpio_level = 0;
    invoke(BUTTON_PRESS_UP);
    fake_gpio_level = 1;
    fake_timestamp_ms = 2000U;
    invoke(BUTTON_PRESS_DOWN);
    fake_timestamp_ms = 2150U;
    invoke(BUTTON_LONG_PRESS_START);
    assert(!power_button_adapter_is_shutdown_requested());
    assert(keypad_delivery_enabled);
    assert(keypad_gate_close_calls == 0U);
    assert(hook_calls == 0U);
    assert(app_event_posts == 0U);

    power_button_adapter_stop();
}


static void test_arm_after_stable_release_then_shutdown(void)
{
    reset_fakes();
    fake_gpio_level = 0;
    assert(power_button_adapter_init(notification_hook, NULL) == ESP_OK);

    assert(power_button_adapter_arm_after_startup() == ESP_OK);
    assert(delay_calls == 1U);
    assert(delayed_ticks == pdMS_TO_TICKS(50U));
    assert(!power_button_adapter_is_shutdown_requested());

    fake_gpio_level = 1;
    fake_timestamp_ms = 3000U;
    invoke(BUTTON_PRESS_DOWN);
    fake_timestamp_ms = 3150U;
    fake_best_effort_result = ESP_ERR_TIMEOUT;
    invoke(BUTTON_LONG_PRESS_START);
    assert(power_button_adapter_is_shutdown_requested());
    assert(!keypad_delivery_enabled);
    assert(keypad_gate_close_calls == 1U);
    assert((keypad_gate_close_bits & 1U) == 0U);
    assert(hook_calls == 1U);
    assert((hook_bits_at_call & 1U) != 0U);
    assert(app_event_posts == 1U);

    fake_gpio_level = 0;
    invoke(BUTTON_PRESS_UP);
    power_button_adapter_wait_for_shutdown_request();
    power_button_adapter_stop();
}


static void test_arm_while_held_waits_for_debounced_release(void)
{
    reset_fakes();
    fake_gpio_level = 1;
    assert(power_button_adapter_init(notification_hook, NULL) == ESP_OK);

    assert(power_button_adapter_arm_after_startup() == ESP_OK);
    assert(delay_calls == 0U);

    fake_timestamp_ms = 4000U;
    invoke(BUTTON_PRESS_DOWN);
    fake_timestamp_ms = 4150U;
    invoke(BUTTON_LONG_PRESS_START);
    assert(!power_button_adapter_is_shutdown_requested());
    assert(keypad_delivery_enabled);
    assert(keypad_gate_close_calls == 0U);

    fake_gpio_level = 0;
    invoke(BUTTON_PRESS_UP);

    fake_gpio_level = 1;
    fake_timestamp_ms = 5000U;
    invoke(BUTTON_PRESS_DOWN);
    fake_timestamp_ms = 5150U;
    invoke(BUTTON_LONG_PRESS_START);
    assert(power_button_adapter_is_shutdown_requested());
    assert(!keypad_delivery_enabled);
    assert(keypad_gate_close_calls == 1U);
    assert((keypad_gate_close_bits & 1U) == 0U);

    fake_gpio_level = 0;
    invoke(BUTTON_PRESS_UP);
    power_button_adapter_stop();
}


int main(void)
{
    test_button_registration_rejects_equal_effective_ticks();
    test_boot_long_press_cannot_latch_before_explicit_arm();
    test_arm_after_stable_release_then_shutdown();
    test_arm_while_held_waits_for_debounced_release();
    puts("power button tests passed");
    return 0;
}
