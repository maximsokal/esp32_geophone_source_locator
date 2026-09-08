#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/event_groups.h"
#include "power_button_input.h"
#include "power_button_monitor.h"
#include "power_button_monitor_internal.h"

static const bool *fake_levels;
static size_t fake_level_count;
static size_t fake_level_index;
static unsigned fake_delay_calls;
static bool stop_after_delay;
static EventBits_t fake_event_bits;

bool power_button_input_is_high(void)
{
    if (fake_levels == NULL || fake_level_count == 0U) {
        return false;
    }

    const size_t index =
        fake_level_index < fake_level_count
            ? fake_level_index
            : fake_level_count - 1U;
    fake_level_index++;
    return fake_levels[index];
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    fake_delay_calls++;
    if (stop_after_delay) {
        power_button_monitor_state()->running = false;
    }
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
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

static void set_levels(const bool *levels, size_t count)
{
    fake_levels = levels;
    fake_level_count = count;
    fake_level_index = 0U;
    fake_delay_calls = 0U;
    stop_after_delay = false;
}

static void test_required_sample_rounding(void)
{
    assert(power_button_required_samples(0U) == 1U);
    assert(power_button_required_samples(1U) == 1U);
    assert(power_button_required_samples(BUTTON_POLL_MS) == 1U);
    assert(power_button_required_samples(BUTTON_POLL_MS + 1U) == 2U);
    assert(power_button_required_samples(BUTTON_PRESS_CONFIRM_MS) == 15U);
}

static void test_wait_requires_consecutive_matching_samples(void)
{
    const bool released[] = {false, false, false};
    set_levels(released, sizeof(released) / sizeof(released[0]));
    power_button_monitor_state()->running = true;
    assert(power_button_wait_for_level(false, 30U, true));
    assert(fake_delay_calls == 3U);

    const bool noisy_release[] = {true, false, false, false};
    set_levels(noisy_release, sizeof(noisy_release) / sizeof(noisy_release[0]));
    power_button_monitor_state()->running = true;
    assert(power_button_wait_for_level(false, 30U, true));
    assert(fake_delay_calls == 4U);
}

static void test_wait_honours_stop_sensitive_exit(void)
{
    const bool held[] = {true, true, true, true};
    set_levels(held, sizeof(held) / sizeof(held[0]));
    power_button_monitor_state()->running = true;
    stop_after_delay = true;
    assert(!power_button_wait_for_level(false, 30U, true));
    assert(fake_delay_calls == 1U);
}

static void test_shutdown_latch_state(void)
{
    power_button_monitor_state_t *state = power_button_monitor_state();
    state->initialized = false;
    state->events = NULL;
    fake_event_bits = POWER_EVENT_SHUTDOWN_REQUESTED;
    assert(!power_button_monitor_is_shutdown_requested());

    state->initialized = true;
    state->events = (EventGroupHandle_t)&fake_event_bits;
    fake_event_bits = 0U;
    assert(!power_button_monitor_is_shutdown_requested());
    fake_event_bits = POWER_EVENT_SHUTDOWN_REQUESTED;
    assert(power_button_monitor_is_shutdown_requested());
}

int main(void)
{
    test_required_sample_rounding();
    test_wait_requires_consecutive_matching_samples();
    test_wait_honours_stop_sensitive_exit();
    test_shutdown_latch_state();
    return 0;
}
