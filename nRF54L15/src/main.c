/*
 * XIAO nRF54L15 initial PlatformIO / Zephyr bring-up.
 *
 * The current firmware still uses the onboard LED as a simple board/toolchain
 * heartbeat, but the node lifecycle state is now production code shared by
 * future main/remote runtime roles.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <nrfx_power.h>

#include "nrf_node_state.h"

#define SLEEP_TIME_MS 1000
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    int ret;
    bool led_is_on = true;
    nrf_node_state_t node_state;

#if defined(CONFIG_NRFX_POWER)
    nrfx_power_constlat_mode_request();
#endif

    nrf_node_state_init(&node_state);

    if (!gpio_is_ready_dt(&led)) {
        return -1;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        return ret;
    }

    if (nrf_node_state_mark_ready(&node_state) != NRF_NODE_STATE_OK) {
        return -1;
    }

    while (1) {
        ret = gpio_pin_set_dt(&led, (int)led_is_on);
        if (ret < 0) {
            return ret;
        }

        led_is_on = !led_is_on;
        k_msleep(SLEEP_TIME_MS);
    }

    return 0;
}
