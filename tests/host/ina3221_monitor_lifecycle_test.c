#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ina3221_driver.h"
#include "ina3221_policy.h"
#include "device_status.h"

#include "../../components/ina3221_monitor/ina3221_monitor.c"

static int s_task_token;
static TaskFunction_t s_worker_entry;
static void *s_worker_argument;
static bool s_fail_task_create;
static unsigned s_task_create_calls;
static unsigned s_notify_give_calls;
static unsigned s_notify_take_calls;
static TickType_t s_last_wait_ticks;
static int64_t s_fake_time_us;
static unsigned s_delay_calls;
static unsigned s_delete_calls;
static unsigned s_critical_enters;
static unsigned s_critical_exits;

typedef enum {
    DELAY_ACTION_NONE = 0,
    DELAY_ACTION_REGISTER_THEN_CLEAR,
    DELAY_ACTION_CLEAR_ON_FIRST,
} delay_action_t;

static delay_action_t s_delay_action;
static bool s_stop_on_wait;

static esp_err_t s_open_result;
static esp_err_t s_read_result;
static unsigned s_open_calls;
static unsigned s_read_calls;
static unsigned s_close_calls;

static unsigned s_classify_calls;
static unsigned s_classifier_update_calls;
static unsigned s_make_decision_calls;

static unsigned s_set_power_calls;
static unsigned s_set_channel_calls;
static unsigned s_set_battery_calls;
static bool s_last_power_valid;
static bool s_all_channels_invalid;
static bool s_last_battery_present;
static bool s_last_battery_percent_valid;
static bool s_last_usb_present;
static bool s_last_charging;
static device_power_source_t s_last_source;
static float s_last_power_voltage;
static float s_last_power_current;

void test_port_enter_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(mux->depth == 0U);
    mux->depth = 1U;
    ++s_critical_enters;
}

void test_port_exit_critical(portMUX_TYPE *mux)
{
    assert(mux != NULL);
    assert(mux->depth == 1U);
    mux->depth = 0U;
    ++s_critical_exits;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id
)
{
    ++s_task_create_calls;
    assert(task != NULL);
    assert(strcmp(name, "ina3221") == 0);
    assert(stack_depth == APP_TASK_STACK_INA3221);
    assert(priority == APP_TASK_PRIORITY_INA3221);
    assert(core_id == APP_TASK_CORE_SERVICES);
    assert(created_task != NULL);

    if (s_fail_task_create) {
        return pdFALSE;
    }

    s_worker_entry = task;
    s_worker_argument = argument;
    *created_task = &s_task_token;
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return &s_task_token;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &s_task_token);
    ++s_notify_give_calls;
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    assert(clear_on_exit == pdTRUE);
    assert(
        ticks_to_wait == pdMS_TO_TICKS(1000U) ||
        ticks_to_wait == pdMS_TO_TICKS(20U)
    );
    s_last_wait_ticks = ticks_to_wait;
    ++s_notify_take_calls;

    if (s_stop_on_wait) {
        portENTER_CRITICAL(&s_state_lock);
        s_stop_requested = true;
        portEXIT_CRITICAL(&s_state_lock);
        return 1U;
    }

    return 0U;
}

int64_t esp_timer_get_time(void)
{
    return s_fake_time_us;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks == pdMS_TO_TICKS(10U));
    ++s_delay_calls;

    if (s_delay_action == DELAY_ACTION_REGISTER_THEN_CLEAR) {
        if (s_delay_calls == 1U) {
            assert(ina3221_monitor_register_current_task());
        }
        else if (s_delay_calls == 2U) {
            ina3221_monitor_clear_current_task();
        }
    }
    else if (
        s_delay_action == DELAY_ACTION_CLEAR_ON_FIRST &&
        s_delay_calls == 1U
    ) {
        ina3221_monitor_clear_current_task();
    }
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    ++s_delete_calls;
}

esp_err_t ina3221_driver_open(ina3221_driver_t *driver)
{
    assert(driver != NULL);
    ++s_open_calls;
    if (s_open_result == ESP_OK) {
        driver->ready = true;
    }
    return s_open_result;
}

void ina3221_driver_close(ina3221_driver_t *driver)
{
    assert(driver != NULL);
    ++s_close_calls;
    driver->ready = false;
}

esp_err_t ina3221_driver_read_channels(
    ina3221_driver_t *driver,
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT]
)
{
    assert(driver != NULL);
    assert(channels != NULL);
    assert(driver->ready);
    ++s_read_calls;

    if (s_read_result != ESP_OK) {
        return s_read_result;
    }

    channels[0] = (ina3221_channel_sample_t) {
        .valid = true,
        .bus_voltage_v = 4.0f,
        .current_a = -0.25f,
    };
    channels[1] = (ina3221_channel_sample_t) {
        .valid = true,
        .bus_voltage_v = 5.1f,
        .current_a = 0.30f,
    };
    channels[2] = (ina3221_channel_sample_t) {
        .valid = true,
        .bus_voltage_v = 5.0f,
        .current_a = 0.10f,
    };
    return ESP_OK;
}

ina3221_power_state_t ina3221_policy_classify_instant(
    const ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT]
)
{
    assert(channels != NULL);
    ++s_classify_calls;
    return INA3221_POWER_STATE_USB_CHARGING;
}

ina3221_power_state_t ina3221_policy_classifier_update(
    ina3221_power_classifier_t *classifier,
    ina3221_power_state_t instantaneous_state
)
{
    assert(classifier != NULL);
    ++s_classifier_update_calls;
    classifier->initialized = true;
    classifier->stable_state = instantaneous_state;
    return instantaneous_state;
}

void ina3221_policy_make_decision(
    const ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT],
    ina3221_power_state_t stable_state,
    ina3221_power_decision_t *decision
)
{
    assert(channels != NULL);
    assert(decision != NULL);
    assert(stable_state == INA3221_POWER_STATE_USB_CHARGING);
    ++s_make_decision_calls;

    *decision = (ina3221_power_decision_t) {
        .state = stable_state,
        .battery_present = true,
        .battery_percent_valid = true,
        .battery_voltage_v = 4.0f,
        .battery_current_a = -0.25f,
        .battery_percent_estimate = 75.0f,
        .usb_present = true,
        .charging = true,
    };
}

const char *ina3221_policy_state_name(ina3221_power_state_t state)
{
    (void)state;
    return "stub";
}

esp_err_t device_status_set_power(float voltage_v, float current_a, bool valid)
{
    ++s_set_power_calls;
    s_last_power_voltage = voltage_v;
    s_last_power_current = current_a;
    s_last_power_valid = valid;
    return ESP_OK;
}

esp_err_t device_status_set_power_channel(
    size_t channel_index,
    float voltage_v,
    float current_a,
    bool valid
)
{
    assert(channel_index < INA3221_POLICY_CHANNEL_COUNT);
    (void)voltage_v;
    (void)current_a;
    ++s_set_channel_calls;
    if (valid) {
        s_all_channels_invalid = false;
    }
    return ESP_OK;
}

esp_err_t device_status_set_power_battery(
    float battery_voltage_v,
    float battery_current_a,
    float battery_percent_estimate,
    bool battery_present,
    bool battery_percent_valid,
    bool usb_present,
    bool charging,
    device_power_source_t source
)
{
    (void)battery_voltage_v;
    (void)battery_current_a;
    (void)battery_percent_estimate;
    ++s_set_battery_calls;
    s_last_battery_present = battery_present;
    s_last_battery_percent_valid = battery_percent_valid;
    s_last_usb_present = usb_present;
    s_last_charging = charging;
    s_last_source = source;
    return ESP_OK;
}

static void reset_fakes(void)
{
    s_worker_entry = NULL;
    s_worker_argument = NULL;
    s_fail_task_create = false;
    s_task_create_calls = 0U;
    s_notify_give_calls = 0U;
    s_notify_take_calls = 0U;
    s_last_wait_ticks = 0U;
    s_fake_time_us = 0;
    s_delay_calls = 0U;
    s_delete_calls = 0U;
    s_critical_enters = 0U;
    s_critical_exits = 0U;
    s_delay_action = DELAY_ACTION_NONE;
    s_stop_on_wait = false;

    s_open_result = ESP_OK;
    s_read_result = ESP_OK;
    s_open_calls = 0U;
    s_read_calls = 0U;
    s_close_calls = 0U;

    s_classify_calls = 0U;
    s_classifier_update_calls = 0U;
    s_make_decision_calls = 0U;

    s_set_power_calls = 0U;
    s_set_channel_calls = 0U;
    s_set_battery_calls = 0U;
    s_last_power_valid = false;
    s_all_channels_invalid = true;
    s_last_battery_present = false;
    s_last_battery_percent_valid = false;
    s_last_usb_present = false;
    s_last_charging = false;
    s_last_source = DEVICE_POWER_SOURCE_UNKNOWN;
    s_last_power_voltage = 0.0f;
    s_last_power_current = 0.0f;

    s_task = NULL;
    s_starting = false;
    s_stop_requested = false;
    s_fast_sampling_deadline_us = 0;
    s_state_lock.depth = 0U;
}

static void assert_lock_balanced(void)
{
    assert(s_state_lock.depth == 0U);
    assert(s_critical_enters == s_critical_exits);
}

static void test_create_failure_rolls_back_and_retries(void)
{
    reset_fakes();
    s_fail_task_create = true;

    assert(ina3221_monitor_start() == ESP_ERR_NO_MEM);
    assert(s_task_create_calls == 1U);
    assert(!s_starting);
    assert(s_task == NULL);
    assert(!s_stop_requested);

    s_fail_task_create = false;
    assert(ina3221_monitor_start() == ESP_OK);
    assert(s_task_create_calls == 2U);
    assert(s_starting);
    assert(s_task == NULL);
    assert(s_worker_entry != NULL);
    assert_lock_balanced();
}

static void test_start_is_idempotent_during_creation_handoff(void)
{
    reset_fakes();

    assert(ina3221_monitor_start() == ESP_OK);
    assert(ina3221_monitor_start() == ESP_OK);
    assert(s_task_create_calls == 1U);
    assert(s_starting);
    assert_lock_balanced();
}

static void test_start_is_rejected_while_stop_is_active(void)
{
    reset_fakes();
    s_task = &s_task_token;
    s_stop_requested = true;

    assert(ina3221_monitor_start() == ESP_ERR_INVALID_STATE);
    assert(s_task_create_calls == 0U);
    assert_lock_balanced();
}

static void test_stop_handles_delayed_worker_registration(void)
{
    reset_fakes();
    assert(ina3221_monitor_start() == ESP_OK);
    assert(s_starting);

    s_delay_action = DELAY_ACTION_REGISTER_THEN_CLEAR;
    ina3221_monitor_stop();

    assert(s_delay_calls == 2U);
    assert(s_notify_give_calls == 0U);
    assert(s_task == NULL);
    assert(!s_starting);
    assert(!s_stop_requested);

    assert(ina3221_monitor_start() == ESP_OK);
    assert(s_task_create_calls == 2U);
    assert_lock_balanced();
}

static void test_stop_notifies_running_worker_once(void)
{
    reset_fakes();
    assert(ina3221_monitor_start() == ESP_OK);
    assert(!ina3221_monitor_register_current_task());
    assert(s_task == &s_task_token);
    assert(!s_starting);

    s_delay_action = DELAY_ACTION_CLEAR_ON_FIRST;
    ina3221_monitor_stop();

    assert(s_notify_give_calls == 1U);
    assert(s_delay_calls == 1U);
    assert(s_task == NULL);
    assert(!s_stop_requested);

    assert(ina3221_monitor_start() == ESP_OK);
    assert(s_task_create_calls == 2U);
    assert_lock_balanced();
}

static void test_worker_publishes_normal_sample_and_cleans_up(void)
{
    reset_fakes();
    assert(ina3221_monitor_start() == ESP_OK);
    s_stop_on_wait = true;

    s_worker_entry(s_worker_argument);

    assert(s_open_calls == 1U);
    assert(s_read_calls == 1U);
    assert(s_close_calls == 1U);
    assert(s_classify_calls == 1U);
    assert(s_classifier_update_calls == 1U);
    assert(s_make_decision_calls == 1U);
    assert(s_set_channel_calls == 3U);
    assert(s_set_power_calls == 1U);
    assert(s_last_power_valid);
    assert(s_last_power_voltage > 5.0f);
    assert(s_last_power_current > 0.0f);
    assert(s_set_battery_calls == 1U);
    assert(s_last_battery_present);
    assert(s_last_battery_percent_valid);
    assert(s_last_usb_present);
    assert(s_last_charging);
    assert(s_last_source == DEVICE_POWER_SOURCE_USB);
    assert(s_notify_take_calls == 1U);
    assert(s_delete_calls == 1U);
    assert(s_task == NULL);
    assert(!s_starting);
    assert_lock_balanced();

    /* MAX-242: the AD7606 rail reading is published with its timestamp. */
    ina3221_monitor_rail_t rail;
    assert(ina3221_monitor_get_rail(INA3221_MONITOR_RAIL_AD7606_5V, &rail) == ESP_OK);
    assert(rail.available && rail.valid);
    assert(rail.sampled_at_us == s_fake_time_us);
    assert(ina3221_monitor_get_rail(3U, &rail) == ESP_ERR_INVALID_ARG);
    assert(ina3221_monitor_get_rail(0U, NULL) == ESP_ERR_INVALID_ARG);
    assert_lock_balanced();
}

static void test_open_failure_publishes_invalid_and_stops_on_notification(void)
{
    reset_fakes();
    s_open_result = ESP_FAIL;
    assert(ina3221_monitor_start() == ESP_OK);
    s_stop_on_wait = true;

    s_worker_entry(s_worker_argument);

    assert(s_open_calls == 1U);
    assert(s_read_calls == 0U);
    assert(s_set_power_calls == 1U);
    assert(!s_last_power_valid);
    assert(s_set_channel_calls == 3U);
    assert(s_all_channels_invalid);
    assert(s_set_battery_calls == 1U);
    assert(!s_last_battery_present);
    assert(!s_last_battery_percent_valid);
    assert(!s_last_usb_present);
    assert(!s_last_charging);
    assert(s_last_source == DEVICE_POWER_SOURCE_UNKNOWN);
    assert(s_notify_take_calls == 1U);
    assert(s_delete_calls == 1U);
    assert(s_task == NULL);
    assert_lock_balanced();
}

static void test_read_failure_invalidates_and_closes_driver(void)
{
    reset_fakes();
    s_read_result = ESP_FAIL;
    assert(ina3221_monitor_start() == ESP_OK);
    s_stop_on_wait = true;

    s_worker_entry(s_worker_argument);

    assert(s_open_calls == 1U);
    assert(s_read_calls == 1U);
    assert(s_close_calls == 2U);
    assert(s_set_power_calls == 1U);
    assert(!s_last_power_valid);
    assert(s_set_channel_calls == 3U);
    assert(s_all_channels_invalid);
    assert(s_set_battery_calls == 1U);
    assert(s_notify_take_calls == 1U);
    assert(s_delete_calls == 1U);
    assert(s_task == NULL);
    assert_lock_balanced();

    /* A failed read publishes an available-but-invalid rail, never the
     * previous good voltage. */
    ina3221_monitor_rail_t rail;
    assert(ina3221_monitor_get_rail(INA3221_MONITOR_RAIL_AD7606_5V, &rail) == ESP_OK);
    assert(rail.available && !rail.valid);
    assert_lock_balanced();
}

static void test_fast_sampling_request_without_running_task_stores_deadline_only(void)
{
    reset_fakes();
    s_fake_time_us = 10;

    ina3221_monitor_request_fast_sampling(1);

    assert(s_notify_give_calls == 0U);
    assert(ina3221_monitor_fast_sampling_active());
    assert_lock_balanced();
}

static void test_fast_sampling_wakes_worker_and_uses_fast_period(void)
{
    reset_fakes();
    assert(ina3221_monitor_start() == ESP_OK);
    assert(!ina3221_monitor_register_current_task());
    assert(s_task == &s_task_token);

    s_fake_time_us = 1000;
    assert(!ina3221_monitor_fast_sampling_active());

    ina3221_monitor_request_fast_sampling(500);

    assert(s_notify_give_calls == 1U);
    assert(ina3221_monitor_fast_sampling_active());

    s_stop_on_wait = true;
    s_worker_entry(s_worker_argument);

    assert(s_last_wait_ticks == pdMS_TO_TICKS(20U));

    s_fake_time_us = 501000;
    assert(!ina3221_monitor_fast_sampling_active());
    assert_lock_balanced();
}

int main(void)
{
    test_create_failure_rolls_back_and_retries();
    test_start_is_idempotent_during_creation_handoff();
    test_start_is_rejected_while_stop_is_active();
    test_stop_handles_delayed_worker_registration();
    test_stop_notifies_running_worker_once();
    test_worker_publishes_normal_sample_and_cleans_up();
    test_open_failure_publishes_invalid_and_stops_on_notification();
    test_read_failure_invalidates_and_closes_driver();
    test_fast_sampling_request_without_running_task_stores_deadline_only();
    test_fast_sampling_wakes_worker_and_uses_fast_period();
    return 0;
}
