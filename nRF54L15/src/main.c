/*
 * XIAO nRF54L15 production bring-up.
 *
 * All boards boot with an UNASSIGNED node role and expose the same upstream
 * bootstrap protocol at the DISCOVERY I2C address. Runtime assignment decides
 * MAIN versus REMOTE; there are no separate production firmware variants.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <stdint.h>
#include <nrfx_power.h>

#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_cs_pair_runtime_zephyr.h"
#include "nrf_node_state.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_watchdog_zephyr.h"

#include <zephyr/sys/printk.h>

#define SLEEP_TIME_MS 1000
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    int ret;
    bool led_is_on = true;
    nrf_node_state_t node_state;

    ret = nrf_log_runtime_zephyr_init();
    if (ret != 0) {
        return ret;
    }

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

    ret = nrf_sensor_upstream_runtime_zephyr_start();
    if (ret < 0) {
        return ret;
    }

    ret = nrf_cs_pair_runtime_zephyr_start();
    if (ret < 0) {
        return ret;
    }

    ret = nrf_bootstrap_runtime_zephyr_start(&node_state);
    if (ret < 0) {
        return ret;
    }

    /* MAX-244: armed last, once every monitored runtime exists. A failure
     * leaves the node running unprotected instead of refusing to boot. */
    ret = nrf_watchdog_zephyr_start();
    if (ret < 0) {
        printk("nrf_watchdog: NOT armed (%d); a hung node will not self-reset\n", ret);
    }

    while (1) {
#if defined(CONFIG_APP_LOG_TEST_EVENTS) && CONFIG_APP_LOG_TEST_EVENTS
        static uint32_t test_event_index;
        for (uint32_t burst_index = 0U; burst_index < 32U; ++burst_index) {
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_LOG_TEST_EVENT,
                ++test_event_index,
                (uint32_t)k_uptime_get());
        }
#endif
        nrf_watchdog_zephyr_service();

        ret = gpio_pin_set_dt(&led, (int)led_is_on);
        if (ret < 0) {
            return ret;
        }

        led_is_on = !led_is_on;
        k_msleep(SLEEP_TIME_MS);
    }

    return 0;
}
