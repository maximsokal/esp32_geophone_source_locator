#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tca9548a_mux.h"
#include "tca9548a_mux_i2c_bus.h"
#include "sensor_bus.h"

typedef struct {
    uint8_t control;
    unsigned write_calls;
    unsigned read_calls;
    unsigned fail_write_call;
    unsigned second_fail_write_call;
    unsigned fail_read_call;
    unsigned second_fail_read_call;
    unsigned forced_read_call;
    uint8_t forced_read_value;
    unsigned forced_read_call2;
    uint8_t forced_read_value2;
} fake_io_t;

static esp_err_t fake_write(
    void *context,
    uint8_t address,
    uint8_t value,
    tca9548a_mux_phase_t phase,
    int channel)
{
    fake_io_t *io = context;
    (void)address;
    (void)phase;
    (void)channel;
    assert(io != NULL);
    ++io->write_calls;
    if (io->fail_write_call == io->write_calls ||
        io->second_fail_write_call == io->write_calls) {
        return ESP_FAIL;
    }
    io->control = value;
    return ESP_OK;
}

static esp_err_t fake_read(
    void *context,
    uint8_t address,
    uint8_t *value,
    uint8_t expected,
    tca9548a_mux_phase_t phase,
    int channel)
{
    fake_io_t *io = context;
    (void)address;
    (void)expected;
    (void)phase;
    (void)channel;
    assert(io != NULL);
    assert(value != NULL);
    ++io->read_calls;
    if (io->fail_read_call == io->read_calls ||
        io->second_fail_read_call == io->read_calls) {
        return ESP_ERR_TIMEOUT;
    }
    if (io->forced_read_call == io->read_calls) {
        *value = io->forced_read_value;
    }
    else if (io->forced_read_call2 == io->read_calls) {
        *value = io->forced_read_value2;
    }
    else {
        *value = io->control;
    }
    return ESP_OK;
}

static tca9548a_mux_t make_mux(fake_io_t *io)
{
    tca9548a_mux_t mux;
    const tca9548a_mux_io_t ops = {
        .write_control = fake_write,
        .read_control = fake_read,
        .context = io,
    };
    assert(tca9548a_mux_init(&mux, &ops) == ESP_OK);
    return mux;
}

static void assert_mux_state(
    const tca9548a_mux_t *mux,
    tca9548a_mux_state_t expected
)
{
    const tca9548a_mux_state_t actual = tca9548a_mux_get_state(mux);
    assert(actual == expected);
}


static int s_i2c_device_token;
static uint8_t s_i2c_control;
static unsigned s_i2c_write_calls;
static unsigned s_i2c_read_calls;

esp_err_t sensor_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data
)
{
    assert(device == &s_i2c_device_token);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    ++s_i2c_write_calls;
    s_i2c_control = data;
    return ESP_OK;
}

esp_err_t sensor_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data
)
{
    assert(device == &s_i2c_device_token);
    assert(mem_address == NULL_I2C_MEM_ADDR);
    assert(data != NULL);
    ++s_i2c_read_calls;
    *data = s_i2c_control;
    return ESP_OK;
}

static void test_i2c_bus_adapter_uses_control_byte_without_register_address(void)
{
    s_i2c_control = 0U;
    s_i2c_write_calls = 0U;
    s_i2c_read_calls = 0U;

    tca9548a_mux_t mux;
    assert(tca9548a_mux_bind_i2c_bus(
        &mux, &s_i2c_device_token, 0x70U) == ESP_OK);
    assert(tca9548a_mux_select(&mux, 6U) == ESP_OK);
    assert(s_i2c_control == 0x40U);
    assert(s_i2c_write_calls == 2U);
    assert(s_i2c_read_calls == 2U);
}

static void test_select_closes_then_opens_one_channel(void)
{
    fake_io_t io = {0};
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_select(&mux, 3U) == ESP_OK);
    assert(io.write_calls == 2U);
    assert(io.read_calls == 2U);
    assert(io.control == 0x08U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_SELECTED);
    assert(tca9548a_mux_get_selected_channel(&mux) == 3);
}

static void test_invalid_channel_does_not_touch_bus(void)
{
    fake_io_t io = {0};
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_select(&mux, 8U) == ESP_ERR_INVALID_ARG);
    assert(io.write_calls == 0U);
    assert(io.read_calls == 0U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_FAULT);
}

static void test_selection_write_failure_recovers_closed(void)
{
    fake_io_t io = {
        .fail_write_call = 2U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_select(&mux, 2U) == ESP_FAIL);
    assert(io.write_calls == 3U);
    assert(io.read_calls == 2U);
    assert(io.control == 0U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_CLOSED);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_NONE);
    assert(tca9548a_mux_get_selected_channel(&mux) == -1);
}

static void test_selection_readback_mismatch_recovers_closed(void)
{
    fake_io_t io = {
        .forced_read_call = 2U,
        .forced_read_value = 0x4U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_select(&mux, 1U) == ESP_ERR_INVALID_RESPONSE);
    assert(io.write_calls == 3U);
    assert(io.read_calls == 3U);
    assert(io.control == 0U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_CLOSED);
}

static void test_failed_recovery_leaves_fault(void)
{
    fake_io_t io = {
        .fail_write_call = 2U,
        .second_fail_write_call = 3U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_select(&mux, 5U) == ESP_FAIL);
    assert(io.write_calls == 3U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_FAULT);
    assert(tca9548a_mux_get_selected_channel(&mux) == -1);
}

static void test_close_first_verify_timeout_retry_ok_returns_ok(void)
{
    fake_io_t io = {
        .fail_read_call = 1U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_close(&mux) == ESP_OK);
    assert(io.write_calls == 2U);
    assert(io.read_calls == 2U);
    assert(io.control == 0U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_CLOSED);
}

static void test_close_both_verifies_fail_latches_fault(void)
{
    fake_io_t io = {
        .fail_read_call = 1U,
        .second_fail_read_call = 2U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_close(&mux) == ESP_ERR_TIMEOUT);
    assert(io.write_calls == 2U);
    assert(io.read_calls == 2U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_FAULT);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_VERIFY_TIMEOUT);
}

static void test_close_wrong_readback_latches_fault(void)
{
    fake_io_t io = {
        .forced_read_call = 1U,
        .forced_read_value = 0x04U,
        .forced_read_call2 = 2U,
        .forced_read_value2 = 0x04U,
    };
    tca9548a_mux_t mux = make_mux(&io);

    assert(tca9548a_mux_close(&mux) == ESP_ERR_INVALID_RESPONSE);
    assert(io.write_calls == 2U);
    assert(io.read_calls == 2U);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_FAULT);
    assert(tca9548a_mux_get_fault_reason(&mux) == TCA9548A_MUX_FAULT_READBACK_MISMATCH);
}

static void test_successful_close_auto_clears_prior_mux_fault(void)
{
    fake_io_t io = {
        .fail_read_call = 1U,
        .second_fail_read_call = 2U,
    };
    tca9548a_mux_t mux = make_mux(&io);
    assert(tca9548a_mux_close(&mux) == ESP_ERR_TIMEOUT);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_FAULT);

    io.fail_read_call = 0U;
    io.second_fail_read_call = 0U;
    assert(tca9548a_mux_close(&mux) == ESP_OK);
    assert_mux_state(&mux, TCA9548A_MUX_STATE_CLOSED);
}

int main(void)
{
    test_i2c_bus_adapter_uses_control_byte_without_register_address();
    test_select_closes_then_opens_one_channel();
    test_invalid_channel_does_not_touch_bus();
    test_selection_write_failure_recovers_closed();
    test_selection_readback_mismatch_recovers_closed();
    test_failed_recovery_leaves_fault();
    test_close_first_verify_timeout_retry_ok_returns_ok();
    test_close_both_verifies_fail_latches_fault();
    test_close_wrong_readback_latches_fault();
    test_successful_close_auto_clears_prior_mux_fault();
    return 0;
}
