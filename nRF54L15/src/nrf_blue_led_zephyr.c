#include "nrf_blue_led.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

static const struct gpio_dt_spec s_blue_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(blue_led), gpios);
static volatile bool s_blue_ready;
static volatile int8_t s_blue_state = -1;
static volatile uint32_t s_blue_failure_count;

static void increment_failure_count(void)
{
    const uint32_t current = s_blue_failure_count;
    if (current != UINT32_MAX) {
        s_blue_failure_count = current + 1U;
    }
}

int nrf_blue_led_zephyr_init(void)
{
    int result;

    s_blue_ready = false;
    s_blue_state = -1;
    if (!gpio_is_ready_dt(&s_blue_led)) {
        return -ENODEV;
    }
    result = gpio_pin_configure_dt(&s_blue_led, GPIO_OUTPUT_INACTIVE);
    if (result != 0) {
        increment_failure_count();
        return result;
    }
    s_blue_ready = true;
    s_blue_state = 0;
    return 0;
}

int nrf_blue_led_zephyr_set_test_pin(bool on)
{
    int result;

    if (!s_blue_ready) {
        return -ENODEV;
    }
    result = gpio_pin_set_dt(&s_blue_led, on ? 1 : 0);
    if (result != 0) {
        s_blue_state = -1;
        increment_failure_count();
        return result;
    }
    s_blue_state = (int8_t)(on ? 1 : 0);
    return 0;
}

int8_t nrf_blue_led_zephyr_state(void)
{
    return s_blue_state;
}

uint32_t nrf_blue_led_zephyr_failure_count(void)
{
    return s_blue_failure_count;
}

void nrf_blue_led_zephyr_update(
    nrf_verticality_state_t state,
    uint64_t uptime_ms)
{
    const nrf_blue_led_action_t action = nrf_blue_led_policy(state);
    const bool on = action == NRF_BLUE_LED_ON ||
        (action == NRF_BLUE_LED_BLINK_2HZ &&
         (uptime_ms % (NRF_BLUE_LED_BLINK_HALF_PERIOD_MS * 2U)) <
             NRF_BLUE_LED_BLINK_HALF_PERIOD_MS);
    (void)nrf_blue_led_zephyr_set_test_pin(on);
}
