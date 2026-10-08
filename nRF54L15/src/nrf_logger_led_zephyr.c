#include "nrf_logger_led_zephyr.h"

#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

static const struct gpio_dt_spec s_green_led =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(logger_green), gpios, {0});
static const struct gpio_dt_spec s_yellow_led =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(logger_yellow), gpios, {0});
static const struct gpio_dt_spec s_red_led =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(logger_red), gpios, {0});

static bool s_green_ready;
static bool s_yellow_ready;
static bool s_red_ready;
static bool s_initialized;
static bool s_failure_logged;
static struct k_spinlock s_diagnostics_lock;
static uint32_t s_failure_count;
static atomic_t s_init_boot_fatal;

static void record_gpio_failure(const char *name, int error)
{
    k_spinlock_key_t key = k_spin_lock(&s_diagnostics_lock);
    bool should_log = !s_failure_logged;

    if (s_failure_count < UINT32_MAX) {
        ++s_failure_count;
    }
    s_failure_logged = true;
    k_spin_unlock(&s_diagnostics_lock, key);

    if (should_log) {
        printk("nrf_logger_led: %s GPIO operation failed (%d)\n", name, error);
    }
}

static nrf_logger_led_zephyr_result_t configure_led(
    const struct gpio_dt_spec *spec,
    bool *configured,
    const char *name)
{
    int result;

    if (spec->port == NULL || !gpio_is_ready_dt(spec)) {
        return NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE;
    }
    result = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
    if (result != 0) {
        record_gpio_failure(name, result);
        return NRF_LOGGER_LED_ZEPHYR_ERR_GPIO;
    }
    *configured = true;
    return NRF_LOGGER_LED_ZEPHYR_OK;
}

static nrf_logger_led_zephyr_result_t apply_led(
    const struct gpio_dt_spec *spec,
    bool *configured,
    bool on,
    const char *name)
{
    int result;

    if (!*configured) {
        return NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE;
    }
    result = gpio_pin_set_dt(spec, on);
    if (result != 0) {
        record_gpio_failure(name, result);
        /* Keep a failed pin disabled until reboot instead of retrying on each event. */
        *configured = false;
        return NRF_LOGGER_LED_ZEPHYR_ERR_GPIO;
    }
    return NRF_LOGGER_LED_ZEPHYR_OK;
}

nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_init(void)
{
    nrf_logger_led_zephyr_result_t green_result;
    nrf_logger_led_zephyr_result_t yellow_result;
    nrf_logger_led_zephyr_result_t red_result;
    nrf_logger_led_zephyr_result_t result = NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE;
    k_spinlock_key_t key;

    s_green_ready = false;
    s_yellow_ready = false;
    s_red_ready = false;
    s_initialized = false;
    key = k_spin_lock(&s_diagnostics_lock);
    s_failure_logged = false;
    s_failure_count = 0U;
    k_spin_unlock(&s_diagnostics_lock, key);
    atomic_set(&s_init_boot_fatal, 0);

    green_result = configure_led(&s_green_led, &s_green_ready, "GREEN");
    yellow_result = configure_led(&s_yellow_led, &s_yellow_ready, "YELLOW");
    red_result = configure_led(&s_red_led, &s_red_ready, "RED");
    if (green_result == NRF_LOGGER_LED_ZEPHYR_OK ||
        yellow_result == NRF_LOGGER_LED_ZEPHYR_OK ||
        red_result == NRF_LOGGER_LED_ZEPHYR_OK) {
        result = NRF_LOGGER_LED_ZEPHYR_OK;
    }
    if (green_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO ||
        yellow_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO ||
        red_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO) {
        result = NRF_LOGGER_LED_ZEPHYR_ERR_GPIO;
    }
    s_initialized = true;
    return result;
}

nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_apply(
    const nrf_logger_led_output_t *output)
{
    nrf_logger_led_zephyr_result_t green_result;
    nrf_logger_led_zephyr_result_t yellow_result;
    nrf_logger_led_zephyr_result_t red_result;
    nrf_logger_led_zephyr_result_t result = NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE;

    if (output == NULL) {
        return NRF_LOGGER_LED_ZEPHYR_ERR_ARGUMENT;
    }
    if (!s_initialized) {
        return NRF_LOGGER_LED_ZEPHYR_ERR_NOT_INITIALIZED;
    }

    green_result = apply_led(&s_green_led, &s_green_ready, output->green, "GREEN");
    yellow_result = apply_led(
        &s_yellow_led, &s_yellow_ready, output->yellow, "YELLOW");
    red_result = apply_led(&s_red_led, &s_red_ready, output->red, "RED");
    if (green_result == NRF_LOGGER_LED_ZEPHYR_OK ||
        yellow_result == NRF_LOGGER_LED_ZEPHYR_OK ||
        red_result == NRF_LOGGER_LED_ZEPHYR_OK) {
        result = NRF_LOGGER_LED_ZEPHYR_OK;
    }
    if (green_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO ||
        yellow_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO ||
        red_result == NRF_LOGGER_LED_ZEPHYR_ERR_GPIO) {
        result = NRF_LOGGER_LED_ZEPHYR_ERR_GPIO;
    }
    return result;
}

uint32_t nrf_logger_led_zephyr_failure_count(void)
{
    k_spinlock_key_t key = k_spin_lock(&s_diagnostics_lock);
    uint32_t count = s_failure_count;

    k_spin_unlock(&s_diagnostics_lock, key);
    return count;
}

void nrf_logger_led_zephyr_latch_init_fatal(void)
{
    atomic_set(&s_init_boot_fatal, 1);
}

bool nrf_logger_led_zephyr_init_fatal(void)
{
    return atomic_get(&s_init_boot_fatal) != 0;
}
