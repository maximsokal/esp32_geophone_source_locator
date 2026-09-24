#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "power_control.h"

/*
 * MAX-246 item 1: behavioural contract of GPIO18, the OFF input of the
 * Pololu power switch. HIGH on this pin cuts the battery; it must only
 * ever go HIGH inside power_control_shutdown(), for exactly OFF_PULSE_MS.
 *
 * Pin model: an output register (level) and a mode per pin, like the ESP32
 * GPIO matrix. The pad is driven only in OUTPUT mode, and then with the
 * register value, so "configure output while the register still holds 1"
 * is visible as a HIGH glitch.
 */

#define OFF_PIN BOARD_POWER_OFF_GPIO
#define EXPECTED_PULSE_MS 200U

static gpio_mode_t s_mode;
static uint32_t s_register_level;
static bool s_pad_high;
static uint32_t s_now_ms;
static uint32_t s_high_since_ms;
static unsigned s_pulses;
static uint32_t s_last_pulse_ms;
static esp_err_t s_config_result;
static esp_err_t s_set_level_result;

/* Order of adapter/pulse events. */
static char s_trace[64];

static void trace(char c)
{
    const size_t n = strlen(s_trace);
    assert(n + 1U < sizeof(s_trace));
    s_trace[n] = c;
    s_trace[n + 1U] = '\0';
}

static void update_pad(void)
{
    const bool high = s_mode == GPIO_MODE_OUTPUT && s_register_level != 0U;
    if (high && !s_pad_high) {
        s_high_since_ms = s_now_ms;
        trace('H');
    } else if (!high && s_pad_high) {
        ++s_pulses;
        s_last_pulse_ms = s_now_ms - s_high_since_ms;
        trace('L');
    }
    s_pad_high = high;
}

static void reset_model(uint32_t stale_register_level)
{
    s_mode = GPIO_MODE_DISABLE;
    s_register_level = stale_register_level;
    s_pad_high = false;
    s_now_ms = 1000U;
    s_high_since_ms = 0U;
    s_pulses = 0U;
    s_last_pulse_ms = 0U;
    s_config_result = ESP_OK;
    s_set_level_result = ESP_OK;
    s_trace[0] = '\0';
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config != NULL);
    /* power_output owns GPIO18 and nothing else. */
    assert(config->pin_bit_mask == (1ULL << OFF_PIN));
    assert(config->pull_up_en == GPIO_PULLUP_DISABLE);
    if (s_config_result != ESP_OK) {
        return s_config_result;
    }
    s_mode = config->mode;
    update_pad();
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    assert(gpio_num == OFF_PIN);
    if (s_set_level_result != ESP_OK) {
        return s_set_level_result;
    }
    s_register_level = level;
    update_pad();
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    s_now_ms += (uint32_t)ticks_to_delay;
}

/* Fake power button adapter (its policy is covered by power_button_policy). */
typedef void (*power_button_shutdown_notification_hook_t)(void *user_context);
static unsigned s_adapter_stop_calls;
static bool s_shutdown_requested;

esp_err_t power_button_adapter_init(power_button_shutdown_notification_hook_t hook, void *user_context)
{
    (void)hook;
    (void)user_context;
    trace('i');
    return ESP_OK;
}

esp_err_t power_button_adapter_arm_after_startup(void)
{
    trace('a');
    return ESP_OK;
}

esp_err_t power_button_adapter_set_notification_hook(power_button_shutdown_notification_hook_t hook, void *user_context)
{
    (void)hook;
    (void)user_context;
    return ESP_OK;
}

void power_button_adapter_wait_for_shutdown_request(void)
{
    trace('w');
}

bool power_button_adapter_is_shutdown_requested(void)
{
    return s_shutdown_requested;
}

static unsigned s_adapter_request_calls;
static esp_err_t s_adapter_request_result = ESP_OK;

esp_err_t power_button_adapter_request_shutdown(void)
{
    ++s_adapter_request_calls;
    if (s_adapter_request_result == ESP_OK) {
        s_shutdown_requested = true;
    }
    return s_adapter_request_result;
}

void power_button_adapter_stop(void)
{
    ++s_adapter_stop_calls;
    trace('s');
}

static void noop_hook(void *context)
{
    (void)context;
}

static void test_boot_drives_off_low_and_never_high(void)
{
    reset_model(0U);
    assert(power_control_init() == ESP_OK);
    assert(s_mode == GPIO_MODE_OUTPUT);
    assert(s_register_level == 0U);
    assert(!s_pad_high);
    assert(s_pulses == 0U);
    assert(strchr(s_trace, 'H') == NULL);
}

static void test_stale_high_register_is_not_glitched_onto_the_pad(void)
{
    /* The output register may still hold 1 (e.g. a restart right after a
     * pulse with USB power present); switching the pin to OUTPUT before
     * loading LOW would cut the battery during boot. */
    reset_model(1U);
    assert(power_control_init() == ESP_OK);
    assert(!s_pad_high);
    assert(s_pulses == 0U);
    assert(strchr(s_trace, 'H') == NULL);
}

static void test_init_failure_never_drives_high(void)
{
    reset_model(1U);
    s_config_result = ESP_FAIL;
    assert(power_control_init() == ESP_FAIL);
    assert(!s_pad_high);
    assert(strchr(s_trace, 'i') == NULL); /* button not initialised either */

    reset_model(0U);
    s_set_level_result = ESP_FAIL;
    assert(power_control_init() == ESP_FAIL);
    assert(!s_pad_high);
}

static void test_runtime_api_never_pulses(void)
{
    reset_model(0U);
    assert(power_control_init() == ESP_OK);
    assert(power_control_set_shutdown_notification_hook(noop_hook, NULL) == ESP_OK);
    assert(power_control_arm_shutdown_button() == ESP_OK);
    s_shutdown_requested = true;
    assert(power_control_is_shutdown_requested());
    power_control_wait_for_shutdown_request();
    s_shutdown_requested = false;
    s_now_ms += 60000U;
    /* A latched request alone is not a pulse: only the explicit shutdown
     * sequence (after the app stopped acquisition/storage) cuts power. */
    assert(s_pulses == 0U);
    assert(!s_pad_high);
}

static void test_shutdown_is_one_exact_pulse_after_button_stop(void)
{
    reset_model(0U);
    assert(power_control_init() == ESP_OK);
    s_trace[0] = '\0';
    s_adapter_stop_calls = 0U;

    power_control_shutdown();

    assert(s_adapter_stop_calls == 1U);
    /* Button stopped first, then exactly one HIGH..LOW pulse. */
    assert(strcmp(s_trace, "sHL") == 0);
    assert(s_pulses == 1U);
    assert(s_last_pulse_ms == EXPECTED_PULSE_MS);
    assert(!s_pad_high);
    assert(s_register_level == 0U);
}

/* MAX-183: a firmware shutdown request (critical battery) uses the same
 * latch, never pulses GPIO18 by itself, and keeps the first reason. */
static void test_low_battery_request_uses_the_latch_without_pulsing(void)
{
    reset_model(0U);
    assert(power_control_init() == ESP_OK);
    s_shutdown_requested = false;
    s_adapter_request_calls = 0U;
    s_adapter_request_result = ESP_OK;

    assert(power_control_shutdown_reason() == POWER_SHUTDOWN_REASON_NONE);
    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_BUTTON) == ESP_ERR_INVALID_ARG);
    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_NONE) == ESP_ERR_INVALID_ARG);
    assert(s_adapter_request_calls == 0U);

    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY) == ESP_OK);
    assert(s_adapter_request_calls == 1U);
    assert(power_control_is_shutdown_requested());
    assert(power_control_shutdown_reason() == POWER_SHUTDOWN_REASON_LOW_BATTERY);
    assert(strcmp(power_control_shutdown_reason_name(POWER_SHUTDOWN_REASON_LOW_BATTERY), "low_battery") == 0);
    /* Requesting is not cutting power: GPIO18 stays LOW until the app's
     * shutdown sequence calls power_control_shutdown(). */
    assert(s_pulses == 0U && !s_pad_high);

    /* Idempotent: a second request neither re-latches nor changes reason. */
    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY) == ESP_OK);
    assert(s_adapter_request_calls == 1U);

    s_shutdown_requested = false;
}

static void test_request_failure_leaves_no_reason(void)
{
    reset_model(0U);
    assert(power_control_init() == ESP_OK);
    s_shutdown_requested = false;
    s_adapter_request_result = ESP_ERR_INVALID_STATE;
    assert(power_control_request_shutdown(POWER_SHUTDOWN_REASON_LOW_BATTERY) == ESP_ERR_INVALID_STATE);
    assert(power_control_shutdown_reason() == POWER_SHUTDOWN_REASON_NONE);
    /* A later button latch is reported as the button. */
    s_adapter_request_result = ESP_OK;
    s_shutdown_requested = true;
    assert(power_control_shutdown_reason() == POWER_SHUTDOWN_REASON_BUTTON);
    s_shutdown_requested = false;
}

int main(void)
{
    test_boot_drives_off_low_and_never_high();
    test_stale_high_register_is_not_glitched_onto_the_pad();
    test_init_failure_never_drives_high();
    test_runtime_api_never_pulses();
    test_shutdown_is_one_exact_pulse_after_button_stop();
    test_low_battery_request_uses_the_latch_without_pulsing();
    test_request_failure_leaves_no_reason();
    return 0;
}
