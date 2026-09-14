/*
 * XIAO nRF54L15 initial PlatformIO / Zephyr bring-up.
 *
 * This firmware is temporary.
 * Its only purpose is to prove that the board definition,
 * Zephyr toolchain and PlatformIO subproject build correctly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <nrfx_power.h>

#define SLEEP_TIME_MS 1000
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led =
    GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    int ret;
    bool led_is_on = true;

#if defined(CONFIG_NRFX_POWER)
    nrfx_power_constlat_mode_request();
#endif

    if (!gpio_is_ready_dt(&led)) {
        return -1;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        return ret;
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