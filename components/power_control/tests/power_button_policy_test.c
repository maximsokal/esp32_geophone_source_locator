#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "button_gpio.h"
#include "driver/gpio.h"
#include "freertos/event_groups.h"
#include "app_events.h"
#include "iot_button.h"


typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);

esp_err_t power_button_adapter_init(
    power_button_shutdown_notification_hook_t hook,
    void *user_context
);
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
    callbacks[event] = callback;
    callback_contexts[event] = user_data;
    return ESP_OK;
}


button_event_t iot_button_get_event(button_handle_t button)
{
    (void)button;
    return fake_event;
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


static void test_startup_held_and_long_press_policy(void)
{
    reset_fakes();
    fake_gpio_level = 1;
    assert(power_button_adapter_init(notification_hook, NULL) == ESP_OK);
    assert(captured_gpio_config.gpio_num == 21);
    assert(captured_gpio_config.active_level == 1U);
    assert(captured_gpio_config.disable_pull);
    assert(captured_button_config.long_press_time == 150U);

    /* Startup-held input is ignored until its release arms the object. */
    invoke(BUTTON_LONG_PRESS_START);
    assert(!power_button_adapter_is_shutdown_requested());
    assert(hook_calls == 0U);

    fake_gpio_level = 0;
    invoke(BUTTON_PRESS_UP);
    fake_gpio_level = 1;
    /* A full APP_LOOP queue must not prevent the authoritative latch or the
     * optional notification hook from completing the shutdown path. */
    fake_best_effort_result = ESP_ERR_TIMEOUT;
    invoke(BUTTON_LONG_PRESS_START);
    assert(power_button_adapter_is_shutdown_requested());
    assert(hook_calls == 1U);
    assert((hook_bits_at_call & 1U) != 0U);
    assert(app_event_posts == 1U);

    /* The adapter waits on Button's debounced release event, not a task. */
    invoke(BUTTON_PRESS_UP);
    power_button_adapter_wait_for_shutdown_request();
    power_button_adapter_stop();
}


int main(void)
{
    test_startup_held_and_long_press_policy();
    puts("power button tests passed");
    return 0;
}
