#include "nrf_blue_led.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

static const struct gpio_dt_spec s_blue_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(blue_led), gpios);

int nrf_blue_led_zephyr_init(void)
{
    if (!gpio_is_ready_dt(&s_blue_led)) {
        return -ENODEV;
    }
    return gpio_pin_configure_dt(&s_blue_led, GPIO_OUTPUT_INACTIVE);
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
    (void)gpio_pin_set_dt(&s_blue_led, on ? 1 : 0);
}
