#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>

#include "nrf_upstream_target_zephyr.h"

const struct device nrf_upstream_target_test_i2c22_device = {0};

static bool s_device_ready = true;
static int s_register_results[4];
static size_t s_register_result_count;
static size_t s_register_result_index;
static int s_unregister_result;
static unsigned int s_register_calls;
static unsigned int s_unregister_calls;
static uint16_t s_registered_addresses[4];
static uint8_t s_hw_id[8];
static size_t s_hw_id_length;
static int s_hw_id_error;

static void reset_fakes(void)
{
    s_device_ready = true;
    memset(s_register_results, 0, sizeof(s_register_results));
    s_register_result_count = 0U;
    s_register_result_index = 0U;
    s_unregister_result = 0;
    s_register_calls = 0U;
    s_unregister_calls = 0U;
    memset(s_registered_addresses, 0, sizeof(s_registered_addresses));
    memset(s_hw_id, 0, sizeof(s_hw_id));
    s_hw_id_length = 0U;
    s_hw_id_error = 0;
}

bool device_is_ready(const struct device *device)
{
    return device == &nrf_upstream_target_test_i2c22_device && s_device_ready;
}

int i2c_target_register(const struct device *device, struct i2c_target_config *config)
{
    int result = 0;
    assert(device == &nrf_upstream_target_test_i2c22_device);
    assert(config != NULL);
    if (s_register_calls < 4U) {
        s_registered_addresses[s_register_calls] = config->address;
    }
    s_register_calls++;
    if (s_register_result_index < s_register_result_count) {
        result = s_register_results[s_register_result_index++];
    }
    return result;
}

int i2c_target_unregister(const struct device *device, struct i2c_target_config *config)
{
    assert(device == &nrf_upstream_target_test_i2c22_device);
    assert(config != NULL);
    s_unregister_calls++;
    return s_unregister_result;
}

ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    size_t copied;
    if (s_hw_id_error != 0) {
        return (ssize_t)s_hw_id_error;
    }
    copied = s_hw_id_length < length ? s_hw_id_length : length;
    memcpy(buffer, s_hw_id, copied);
    return (ssize_t)copied;
}

static void fake_write_received(
    struct i2c_target_config *config,
    uint8_t *ptr,
    uint32_t len)
{
    (void)config;
    (void)ptr;
    (void)len;
}

static int fake_read_requested(
    struct i2c_target_config *config,
    uint8_t **ptr,
    uint32_t *len)
{
    (void)config;
    (void)ptr;
    (void)len;
    return 0;
}

static const struct i2c_target_callbacks s_callbacks = {
    .buf_write_received = fake_write_received,
    .buf_read_requested = fake_read_requested,
};

static void ensure_unregistered(void)
{
    if (nrf_upstream_target_zephyr_is_registered()) {
        s_unregister_result = 0;
        assert(nrf_upstream_target_zephyr_unregister() == 0);
    }
}

static void reset_test_state(void)
{
    reset_fakes();
    ensure_unregistered();
    reset_fakes();
}

static void test_init_validates_callbacks_and_device(void)
{
    struct i2c_target_callbacks missing_write = s_callbacks;
    reset_test_state();

    assert(nrf_upstream_target_zephyr_init(NULL) == -EINVAL);
    missing_write.buf_write_received = NULL;
    assert(nrf_upstream_target_zephyr_init(&missing_write) == -EINVAL);

    s_device_ready = false;
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == -ENODEV);

    s_device_ready = true;
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);
}

static void test_register_is_7bit_and_idempotent_for_same_address(void)
{
    reset_test_state();
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);

    assert(nrf_upstream_target_zephyr_register(0x80U) == -EINVAL);
    assert(s_register_calls == 0U);

    assert(nrf_upstream_target_zephyr_register(0x35U) == 0);
    assert(nrf_upstream_target_zephyr_is_registered());
    assert(nrf_upstream_target_zephyr_address() == 0x35U);
    assert(s_register_calls == 1U);
    assert(s_registered_addresses[0] == 0x35U);

    assert(nrf_upstream_target_zephyr_register(0x35U) == 0);
    assert(s_register_calls == 1U);
    assert(nrf_upstream_target_zephyr_register(0x36U) == -EBUSY);
    assert(s_register_calls == 1U);
}

static void test_unregister_is_idempotent(void)
{
    reset_test_state();
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);
    assert(nrf_upstream_target_zephyr_register(0x22U) == 0);
    assert(nrf_upstream_target_zephyr_unregister() == 0);
    assert(!nrf_upstream_target_zephyr_is_registered());
    assert(nrf_upstream_target_zephyr_address() == 0U);
    assert(s_unregister_calls == 1U);
    assert(nrf_upstream_target_zephyr_unregister() == 0);
    assert(s_unregister_calls == 1U);
}

static void test_readdress_success_and_unregister_failure(void)
{
    reset_test_state();
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);
    assert(nrf_upstream_target_zephyr_register(0x21U) == 0);

    assert(nrf_upstream_target_zephyr_readdress(0x2AU) == 0);
    assert(nrf_upstream_target_zephyr_address() == 0x2AU);
    assert(s_unregister_calls == 1U);
    assert(s_register_calls == 2U);
    assert(s_registered_addresses[1] == 0x2AU);

    s_unregister_result = -EIO;
    assert(nrf_upstream_target_zephyr_readdress(0x2BU) == -EIO);
    assert(nrf_upstream_target_zephyr_is_registered());
    assert(nrf_upstream_target_zephyr_address() == 0x2AU);
}

static void test_readdress_rolls_back_failed_new_address(void)
{
    reset_test_state();
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);
    assert(nrf_upstream_target_zephyr_register(0x31U) == 0);

    s_register_results[0] = -EIO;
    s_register_results[1] = 0;
    s_register_result_count = 2U;
    assert(nrf_upstream_target_zephyr_readdress(0x32U) == -EIO);
    assert(nrf_upstream_target_zephyr_is_registered());
    assert(nrf_upstream_target_zephyr_address() == 0x31U);
    assert(s_registered_addresses[1] == 0x32U);
    assert(s_registered_addresses[2] == 0x31U);
}

static void test_readdress_exposes_failed_rollback_as_unregistered(void)
{
    reset_test_state();
    assert(nrf_upstream_target_zephyr_init(&s_callbacks) == 0);
    assert(nrf_upstream_target_zephyr_register(0x41U) == 0);

    s_register_results[0] = -EIO;
    s_register_results[1] = -EIO;
    s_register_result_count = 2U;
    assert(nrf_upstream_target_zephyr_readdress(0x42U) == -EIO);
    assert(!nrf_upstream_target_zephyr_is_registered());
    assert(nrf_upstream_target_zephyr_address() == 0U);
}

static void test_device_id_contract(void)
{
    uint8_t buffer[8] = {0};
    size_t length = 99U;

    reset_test_state();
    s_hw_id[0] = 0x12U;
    s_hw_id[1] = 0x34U;
    s_hw_id[2] = 0x56U;
    s_hw_id_length = 3U;

    assert(nrf_upstream_target_zephyr_read_device_id(NULL, sizeof(buffer), &length) == -EINVAL);
    assert(nrf_upstream_target_zephyr_read_device_id(buffer, 0U, &length) == -EINVAL);
    assert(nrf_upstream_target_zephyr_read_device_id(buffer, sizeof(buffer), NULL) == -EINVAL);

    assert(nrf_upstream_target_zephyr_read_device_id(buffer, sizeof(buffer), &length) == 0);
    assert(length == 3U);
    assert(buffer[0] == 0x12U && buffer[1] == 0x34U && buffer[2] == 0x56U);

    s_hw_id_error = -ENOSYS;
    length = 99U;
    assert(nrf_upstream_target_zephyr_read_device_id(buffer, sizeof(buffer), &length) == -ENOSYS);
    assert(length == 0U);

    s_hw_id_error = 0;
    s_hw_id_length = 0U;
    assert(nrf_upstream_target_zephyr_read_device_id(buffer, sizeof(buffer), &length) == -EIO);
}

int main(void)
{
    test_init_validates_callbacks_and_device();
    test_register_is_7bit_and_idempotent_for_same_address();
    test_unregister_is_idempotent();
    test_readdress_success_and_unregister_failure();
    test_readdress_rolls_back_failed_new_address();
    test_readdress_exposes_failed_rollback_as_unregistered();
    test_device_id_contract();
    ensure_unregistered();
    return 0;
}
