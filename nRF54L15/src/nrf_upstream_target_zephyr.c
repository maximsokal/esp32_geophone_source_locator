#include "nrf_upstream_target_zephyr.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/i2c.h>

#if defined(CONFIG_I2C_TARGET)
#include <zephyr/devicetree/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/nrf-pinctrl.h>
#include <zephyr/sys/printk.h>

#define NRF_UPSTREAM_I2C22_PIN_GROUP \
    DT_CHILD(DT_NODELABEL(i2c22_default), group1)
#define NRF_UPSTREAM_I2C22_SDA_LINEAR \
    (DT_PROP_BY_IDX(NRF_UPSTREAM_I2C22_PIN_GROUP, psels, 0) & NRF_PIN_MSK)
#define NRF_UPSTREAM_I2C22_SCL_LINEAR \
    (DT_PROP_BY_IDX(NRF_UPSTREAM_I2C22_PIN_GROUP, psels, 1) & NRF_PIN_MSK)

static void report_registration(uint16_t address, int result)
{
    printk(
        "twis: i2c22 target register bus=i2c22 address=0x%02x ret=%d "
        "sda=P%u.%02u scl=P%u.%02u\n",
        (unsigned int)address,
        result,
        (unsigned int)(NRF_UPSTREAM_I2C22_SDA_LINEAR / 32U),
        (unsigned int)(NRF_UPSTREAM_I2C22_SDA_LINEAR % 32U),
        (unsigned int)(NRF_UPSTREAM_I2C22_SCL_LINEAR / 32U),
        (unsigned int)(NRF_UPSTREAM_I2C22_SCL_LINEAR % 32U));
}
#else
static void report_registration(uint16_t address, int result)
{
    (void)address;
    (void)result;
}
#endif

#define NRF_UPSTREAM_TARGET_MAX_7BIT_ADDRESS 0x7FU

typedef struct {
    const struct device *device;
    struct i2c_target_config config;
    bool initialized;
    bool registered;
    uint16_t address;
} nrf_upstream_target_zephyr_state_t;

static nrf_upstream_target_zephyr_state_t s_target = {
    .device = DEVICE_DT_GET(DT_NODELABEL(i2c22)),
};

static bool callbacks_are_valid(const struct i2c_target_callbacks *callbacks)
{
    return callbacks != NULL &&
           callbacks->buf_write_received != NULL &&
           callbacks->buf_read_requested != NULL;
}

static bool address_is_valid(uint16_t address)
{
    return address <= NRF_UPSTREAM_TARGET_MAX_7BIT_ADDRESS;
}

int nrf_upstream_target_zephyr_init(const struct i2c_target_callbacks *callbacks)
{
    if (!callbacks_are_valid(callbacks)) {
        return -EINVAL;
    }
    if (s_target.registered) {
        return -EBUSY;
    }
    if (s_target.device == NULL || !device_is_ready(s_target.device)) {
        return -ENODEV;
    }

    memset(&s_target.config, 0, sizeof(s_target.config));
    s_target.config.callbacks = callbacks;
    s_target.initialized = true;
    s_target.address = 0U;
    return 0;
}

int nrf_upstream_target_zephyr_register(uint16_t address)
{
    int result;

    if (!s_target.initialized) {
        return -EPERM;
    }
    if (!address_is_valid(address)) {
        return -EINVAL;
    }
    if (s_target.registered) {
        return s_target.address == address ? 0 : -EBUSY;
    }

    s_target.config.flags = 0U;
    s_target.config.address = address;
    result = i2c_target_register(s_target.device, &s_target.config);
    report_registration(address, result);
    if (result != 0) {
        return result;
    }

    s_target.registered = true;
    s_target.address = address;
    return 0;
}

int nrf_upstream_target_zephyr_unregister(void)
{
    int result;

    if (!s_target.initialized) {
        return -EPERM;
    }
    if (!s_target.registered) {
        return 0;
    }

    result = i2c_target_unregister(s_target.device, &s_target.config);
    if (result != 0) {
        return result;
    }

    s_target.registered = false;
    return 0;
}

int nrf_upstream_target_zephyr_readdress(uint16_t address)
{
    int result;
    int rollback_result;
    uint16_t old_address;

    if (!s_target.initialized) {
        return -EPERM;
    }
    if (!address_is_valid(address)) {
        return -EINVAL;
    }
    if (!s_target.registered) {
        return -EPERM;
    }
    if (s_target.address == address) {
        return 0;
    }

    old_address = s_target.address;
    result = i2c_target_unregister(s_target.device, &s_target.config);
    if (result != 0) {
        return result;
    }
    s_target.registered = false;

    s_target.config.address = address;
    result = i2c_target_register(s_target.device, &s_target.config);
    report_registration(address, result);
    if (result == 0) {
        s_target.registered = true;
        s_target.address = address;
        return 0;
    }

    /* Best-effort rollback keeps the previously working address when the
     * new target address cannot be installed.
     */
    s_target.config.address = old_address;
    rollback_result = i2c_target_register(s_target.device, &s_target.config);
    report_registration(old_address, rollback_result);
    if (rollback_result == 0) {
        s_target.registered = true;
        s_target.address = old_address;
        return result;
    }

    s_target.address = 0U;
    return rollback_result;
}

int nrf_upstream_target_zephyr_read_device_id(
    uint8_t *buffer,
    size_t capacity,
    size_t *out_length)
{
    ssize_t copied;

    if (buffer == NULL || capacity == 0U || out_length == NULL) {
        return -EINVAL;
    }

    *out_length = 0U;
    copied = hwinfo_get_device_id(buffer, capacity);
    if (copied < 0) {
        return (int)copied;
    }
    if (copied == 0) {
        return -EIO;
    }

    *out_length = (size_t)copied;
    return 0;
}

bool nrf_upstream_target_zephyr_is_registered(void)
{
    return s_target.registered;
}

uint16_t nrf_upstream_target_zephyr_address(void)
{
    return s_target.registered ? s_target.address : 0U;
}
