#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ad7606_driver.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * MAX-246 item 3: ad7606_presence runtime (GPIO/ISR wiring, start timeout,
 * staleness) against a simulated BUSY line. The observer policy itself is
 * covered by ad7606_presence_policy.
 *
 * Clock: FreeRTOS ticks of 10 ms (CONFIG_FREERTOS_HZ=100); esp_timer
 * follows the tick. On every elapsed tick the simulated ADC produces its
 * waveform, delivered through the registered ISR exactly like ANYEDGE
 * interrupts: only while a handler is added and the interrupt enabled.
 */

typedef enum {
    BUSY_STUCK_LOW = 0,  /* no ADC, pull-down holds the line */
    BUSY_STUCK_HIGH,     /* shorted / floating high */
    BUSY_CONVERTING,     /* one LOW-HIGH-LOW conversion per tick */
} busy_mode_t;

static busy_mode_t s_mode;
static int s_pulse_budget;          /* -1 = unlimited */
static int s_level;
static uint32_t s_tick;
static int s_critical_depth;

static gpio_isr_t s_isr;
static bool s_intr_enabled;
static gpio_int_type_t s_intr_type;
static gpio_pull_mode_t s_pull;
static bool s_pull_set;
static unsigned s_install_calls;
static esp_err_t s_install_result;
static esp_err_t s_add_result;
static esp_err_t s_enable_result;
static unsigned s_remove_calls;
static unsigned s_disable_calls;

static void reset_world(busy_mode_t mode, int pulse_budget)
{
    s_mode = mode;
    s_pulse_budget = pulse_budget;
    s_level = mode == BUSY_STUCK_HIGH ? 1 : 0;
    s_tick = 100U;
    s_critical_depth = 0;
    s_intr_enabled = false;
    s_isr = NULL;
    s_intr_type = GPIO_INTR_DISABLE;
    s_pull_set = false;
    s_add_result = ESP_OK;
    s_enable_result = ESP_OK;
    s_remove_calls = 0U;
    s_disable_calls = 0U;
}

void test_port_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 0);
    ++s_critical_depth;
}

void test_port_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    assert(s_critical_depth == 1);
    --s_critical_depth;
}

static void drive(int level)
{
    if (level == s_level) {
        return;
    }
    s_level = level;
    if (s_isr != NULL && s_intr_enabled && s_intr_type == GPIO_INTR_ANYEDGE) {
        s_isr(NULL);
    }
}

static void simulate_tick(void)
{
    if (s_mode != BUSY_CONVERTING || s_pulse_budget == 0) {
        return;
    }
    drive(1);
    drive(0);
    if (s_pulse_budget > 0) {
        --s_pulse_budget;
    }
}

void vTaskDelay(TickType_t ticks)
{
    assert(s_critical_depth == 0); /* never block inside the spinlock */
    for (TickType_t i = 0; i < ticks; ++i) {
        ++s_tick;
        simulate_tick();
    }
}

TickType_t xTaskGetTickCount(void) { return s_tick; }
TickType_t xTaskGetTickCountFromISR(void) { return s_tick; }
int64_t esp_timer_get_time(void) { return (int64_t)s_tick * 10000LL; }

int gpio_get_level(gpio_num_t gpio_num)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    return s_level;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    s_pull = pull;
    s_pull_set = true;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    ++s_install_calls;
    return s_install_result;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t type)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    s_intr_type = type;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr, void *args)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    assert(s_critical_depth == 0);
    (void)args;
    if (s_add_result != ESP_OK) {
        return s_add_result;
    }
    assert(s_isr == NULL); /* never double-registered */
    s_isr = isr;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    ++s_remove_calls;
    s_isr = NULL;
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    if (s_enable_result != ESP_OK) {
        return s_enable_result;
    }
    s_intr_enabled = true;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num)
{
    assert(gpio_num == BOARD_AD7606_BUSY_GPIO);
    ++s_disable_calls;
    s_intr_enabled = false;
    return ESP_OK;
}

static void assert_monitor_released(void)
{
    assert(s_isr == NULL);
    assert(!s_intr_enabled);
    assert(!ad7606_driver_presence_is_healthy());
}

static void test_converting_adc_is_detected_and_healthy(void)
{
    reset_world(BUSY_CONVERTING, -1);
    const uint32_t start_tick = s_tick;
    assert(ad7606_driver_presence_monitor_start() == ESP_OK);
    /* Pull-down fallback so a missing ADC cannot float into a detection. */
    assert(s_pull_set && s_pull == GPIO_PULLDOWN_ONLY);
    assert(s_intr_type == GPIO_INTR_ANYEDGE);
    assert(s_isr != NULL && s_intr_enabled);
    /* Three complete pulses at one per tick. */
    assert(s_tick - start_tick == 3U);
    assert(ad7606_driver_presence_is_healthy());

    /* Pulses stop (ADC lost): healthy for 250 ms, then stale. */
    s_pulse_budget = 0;
    vTaskDelay(pdMS_TO_TICKS(250));
    assert(ad7606_driver_presence_is_healthy());
    vTaskDelay(1);
    assert(!ad7606_driver_presence_is_healthy());

    ad7606_driver_presence_monitor_stop();
    assert_monitor_released();
}

static void test_missing_adc_times_out_and_releases_the_isr(void)
{
    const busy_mode_t dead[] = { BUSY_STUCK_LOW, BUSY_STUCK_HIGH };
    for (size_t i = 0; i < 2U; ++i) {
        reset_world(dead[i], -1);
        const uint32_t start_tick = s_tick;
        assert(ad7606_driver_presence_monitor_start() == ESP_ERR_NOT_FOUND);
        /* 75 ms start timeout, measured in whole 10 ms ticks. */
        assert(s_tick - start_tick >= 7U && s_tick - start_tick <= 9U);
        assert_monitor_released();
    }
}

static void test_too_few_pulses_are_not_a_device(void)
{
    reset_world(BUSY_CONVERTING, 2);
    assert(ad7606_driver_presence_monitor_start() == ESP_ERR_NOT_FOUND);
    assert_monitor_released();
}

static void test_restart_does_not_reuse_old_pulses(void)
{
    reset_world(BUSY_CONVERTING, -1);
    assert(ad7606_driver_presence_monitor_start() == ESP_OK);
    ad7606_driver_presence_monitor_stop();

    /* Only two new pulses: the three from the previous run must not count. */
    s_pulse_budget = 2;
    assert(ad7606_driver_presence_monitor_start() == ESP_ERR_NOT_FOUND);
    assert_monitor_released();
}

static void test_registration_failures_leave_nothing_armed(void)
{
    reset_world(BUSY_CONVERTING, -1);
    s_add_result = ESP_ERR_INVALID_STATE;
    assert(ad7606_driver_presence_monitor_start() == ESP_ERR_INVALID_STATE);
    assert_monitor_released();

    reset_world(BUSY_CONVERTING, -1);
    s_enable_result = ESP_ERR_INVALID_ARG;
    assert(ad7606_driver_presence_monitor_start() == ESP_ERR_INVALID_ARG);
    assert(s_remove_calls == 1U);
    assert_monitor_released();
}

static void test_isr_service_installed_once_and_shared_service_accepted(void)
{
    /* The first start in this process already ran; the singleton service is
     * never installed again (other drivers may share it). */
    const unsigned before = s_install_calls;
    reset_world(BUSY_CONVERTING, -1);
    assert(ad7606_driver_presence_monitor_start() == ESP_OK);
    assert(s_install_calls == before);
    ad7606_driver_presence_monitor_stop();
}

static void test_stop_is_idempotent(void)
{
    reset_world(BUSY_STUCK_LOW, -1);
    ad7606_driver_presence_monitor_stop();
    ad7606_driver_presence_monitor_stop();
    /* Stopping an inactive monitor does not touch the GPIO driver. */
    assert(s_remove_calls == 0U && s_disable_calls == 0U);
    assert(!ad7606_driver_presence_is_healthy());
}

int main(void)
{
    /* A service installed by another driver is accepted on first use. */
    s_install_result = ESP_ERR_INVALID_STATE;
    test_converting_adc_is_detected_and_healthy();
    assert(s_install_calls == 1U);
    s_install_result = ESP_OK;

    test_missing_adc_times_out_and_releases_the_isr();
    test_too_few_pulses_are_not_a_device();
    test_restart_does_not_reuse_old_pulses();
    test_registration_failures_leave_nothing_armed();
    test_isr_service_installed_once_and_shared_service_accepted();
    test_stop_is_idempotent();
    return 0;
}
