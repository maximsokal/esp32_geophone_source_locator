#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_gpio.h>
#include <zephyr/pm/device_runtime.h>

#include "nrf_line_sampling_stub.h"

#define SDA_PIN NRF_GPIO_PIN_MAP(0, 4)
#define SCL_PIN NRF_GPIO_PIN_MAP(0, 3)

static bool s_suspended = true; /* the driver leaves the controller suspended between transfers */
static bool s_sda_high = true;
static bool s_scl_high = true;
static bool s_fail_resume;
static unsigned s_resume_count;
static unsigned s_suspend_count;
static unsigned s_reads_while_suspended;

void test_line_stub_reset(void)
{
    s_suspended = true;
    s_sda_high = true;
    s_scl_high = true;
    s_fail_resume = false;
    s_resume_count = 0U;
    s_suspend_count = 0U;
    s_reads_while_suspended = 0U;
}

void test_line_stub_set_levels(bool sda_high, bool scl_high)
{
    s_sda_high = sda_high;
    s_scl_high = scl_high;
}

void test_line_stub_fail_resume(bool fail) { s_fail_resume = fail; }
unsigned test_line_stub_resume_count(void) { return s_resume_count; }
unsigned test_line_stub_suspend_count(void) { return s_suspend_count; }
unsigned test_line_stub_reads_while_suspended(void) { return s_reads_while_suspended; }

int pm_device_runtime_get(const struct device *dev)
{
    (void)dev;
    if (s_fail_resume) return -EIO;
    s_suspended = false;
    ++s_resume_count;
    return 0;
}

int pm_device_runtime_put(const struct device *dev)
{
    (void)dev;
    s_suspended = true;
    ++s_suspend_count;
    return 0;
}

uint32_t nrf_gpio_pin_read(uint32_t pin_number)
{
    if (s_suspended) {
        ++s_reads_while_suspended;
        return 0U; /* input buffer disconnected by the sleep pinctrl state */
    }
    if (pin_number == SDA_PIN) return s_sda_high ? 1U : 0U;
    if (pin_number == SCL_PIN) return s_scl_high ? 1U : 0U;
    return 0U;
}
