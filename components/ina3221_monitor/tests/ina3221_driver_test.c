#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ina3221_driver.h"

/*
 * MAX-246 item 4: INA3221 driver against a register model of the chip.
 * Registers are 16-bit big-endian on the wire. Every bus access can be
 * made to fail by its ordinal number, so each error path is reached.
 */

static int s_bus_token;
static int s_device_token;
static uint16_t s_regs[0x100];
static unsigned s_accesses;
static unsigned s_fail_access;          /* 1-based; 0 = never */
static esp_err_t s_fail_err;
static bool s_config_ignores_writes;    /* chip keeps its old config */
static esp_err_t s_get_handles_err;
static unsigned s_writes;
static uint8_t s_last_write_reg;
static uint8_t s_last_write_bytes[2];

static void reset_chip(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[0x00] = 0x7127U; /* INA3221 power-on default config */
    s_accesses = 0U;
    s_fail_access = 0U;
    s_fail_err = ESP_ERR_TIMEOUT;
    s_config_ignores_writes = false;
    s_get_handles_err = ESP_OK;
    s_writes = 0U;
}

static esp_err_t access_result(void)
{
    ++s_accesses;
    return s_accesses == s_fail_access ? s_fail_err : ESP_OK;
}

esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles)
{
    assert(handles != NULL);
    if (s_get_handles_err != ESP_OK) {
        return s_get_handles_err;
    }
    handles->bus = &s_bus_token;
    handles->ina3221 = &s_device_token;
    return ESP_OK;
}

esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data)
{
    assert(device == &s_device_token);
    assert(data_len == 2U);
    const esp_err_t err = access_result();
    if (err != ESP_OK) {
        return err;
    }
    data[0] = (uint8_t)(s_regs[mem_address] >> 8);
    data[1] = (uint8_t)(s_regs[mem_address] & 0xFFU);
    return ESP_OK;
}

esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data)
{
    assert(device == &s_device_token);
    assert(data_len == 2U);
    const esp_err_t err = access_result();
    if (err != ESP_OK) {
        return err;
    }
    ++s_writes;
    s_last_write_reg = mem_address;
    memcpy(s_last_write_bytes, data, 2U);
    /* The driver only ever writes the configuration register. */
    assert(mem_address == 0x00U);
    if (!s_config_ignores_writes) {
        s_regs[mem_address] = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    }
    return ESP_OK;
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-5f;
}

static uint16_t shunt_counts(int counts)
{
    /* Signed 13-bit value in bits 15..3. */
    return (uint16_t)((int16_t)(counts * 8));
}

static uint16_t bus_counts(unsigned counts)
{
    return (uint16_t)(counts << 3);
}

static void open_ok(ina3221_driver_t *driver)
{
    memset(driver, 0, sizeof(*driver));
    assert(ina3221_driver_open(driver) == ESP_OK);
}

static void test_open_writes_exact_config_and_verifies(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);
    assert(driver.ready);
    /* 3 channels, AVG 16, 1.1 ms / 1.1 ms, continuous shunt+bus = 0x7527,
     * sent MSB first. */
    assert(s_writes == 1U);
    assert(s_last_write_reg == 0x00U);
    assert(s_last_write_bytes[0] == 0x75U && s_last_write_bytes[1] == 0x27U);
    assert(s_regs[0x00] == 0x7527U);
    /* probe read, config write, readback */
    assert(s_accesses == 3U);
}

static void test_open_fails_closed_on_every_bus_error(void)
{
    for (unsigned failing = 1U; failing <= 3U; ++failing) {
        reset_chip();
        s_fail_access = failing;
        ina3221_driver_t driver;
        memset(&driver, 0, sizeof(driver));
        assert(ina3221_driver_open(&driver) == ESP_ERR_TIMEOUT);
        assert(!driver.ready);
        assert(driver.device == NULL && driver.bus == NULL);
        ina3221_channel_sample_t ch[INA3221_POLICY_CHANNEL_COUNT];
        assert(ina3221_driver_read_channels(&driver, ch) == ESP_ERR_INVALID_STATE);
    }

    reset_chip();
    s_get_handles_err = ESP_ERR_NO_MEM;
    ina3221_driver_t driver;
    memset(&driver, 0, sizeof(driver));
    assert(ina3221_driver_open(&driver) == ESP_ERR_NO_MEM);
    assert(!driver.ready);
    assert(s_accesses == 0U);
    assert(ina3221_driver_open(NULL) == ESP_ERR_INVALID_ARG);
}

static void test_open_rejects_config_readback_mismatch(void)
{
    /* A different device at 0x40, or a chip that did not latch the write,
     * must not be reported as a working power monitor. */
    reset_chip();
    s_config_ignores_writes = true;
    ina3221_driver_t driver;
    memset(&driver, 0, sizeof(driver));
    assert(ina3221_driver_open(&driver) == ESP_ERR_INVALID_RESPONSE);
    assert(!driver.ready);
    assert(driver.device == NULL);
}

static void test_read_channels_addressing_scaling_and_signs(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);

    /* CH1 battery: +10 mV shunt, 3.696 V bus (462 counts * 8 mV).
     * CH2 ESP32 5 V: shunt reads -10 mV (rail shunts are installed
     * load->source), 5.000 V. CH3 AD7606 5 V: -4 mV, 4.800 V. Distinct
     * values per register catch swapped/misaddressed registers. */
    s_regs[0x01] = shunt_counts(250);
    s_regs[0x02] = bus_counts(462U);
    s_regs[0x03] = shunt_counts(-250);
    s_regs[0x04] = bus_counts(625U);
    s_regs[0x05] = shunt_counts(-100);
    s_regs[0x06] = bus_counts(600U);

    ina3221_channel_sample_t ch[INA3221_POLICY_CHANNEL_COUNT];
    assert(ina3221_driver_read_channels(&driver, ch) == ESP_OK);

    assert(ch[0].valid && ch[1].valid && ch[2].valid);
    assert(near(ch[0].shunt_voltage_v, 0.010f));
    assert(near(ch[0].bus_voltage_v, 3.696f));
    assert(near(ch[0].current_a, 0.100f));      /* battery keeps its sign */
    assert(near(ch[1].shunt_voltage_v, -0.010f));
    assert(near(ch[1].bus_voltage_v, 5.000f));
    assert(near(ch[1].current_a, 0.100f));      /* rail shunt reversed */
    assert(near(ch[2].shunt_voltage_v, -0.004f));
    assert(near(ch[2].bus_voltage_v, 4.800f));
    assert(near(ch[2].current_a, 0.040f));

    /* Low 3 bits of both registers are flags, not data. */
    s_regs[0x02] = (uint16_t)(bus_counts(462U) | 0x7U);
    s_regs[0x01] = (uint16_t)(shunt_counts(250) | 0x7U);
    assert(ina3221_driver_read_channels(&driver, ch) == ESP_OK);
    assert(near(ch[0].bus_voltage_v, 3.696f));
    assert(near(ch[0].shunt_voltage_v, 0.010f));
}

static void test_read_channels_propagates_any_bus_error(void)
{
    /* 6 register reads per sample; each failing position aborts. */
    for (unsigned failing = 1U; failing <= 6U; ++failing) {
        reset_chip();
        ina3221_driver_t driver;
        open_ok(&driver);
        s_fail_access = s_accesses + failing;
        s_fail_err = ESP_FAIL;
        ina3221_channel_sample_t ch[INA3221_POLICY_CHANNEL_COUNT];
        assert(ina3221_driver_read_channels(&driver, ch) == ESP_FAIL);
        /* The channel whose read failed, and every later one, is never
         * marked valid. */
        for (unsigned c = (failing - 1U) / 2U; c < INA3221_POLICY_CHANNEL_COUNT; ++c) {
            assert(!ch[c].valid);
        }
    }
}

static void test_close_and_guards(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);
    ina3221_driver_close(&driver);
    assert(!driver.ready && driver.device == NULL && driver.bus == NULL);
    ina3221_channel_sample_t ch[INA3221_POLICY_CHANNEL_COUNT];
    assert(ina3221_driver_read_channels(&driver, ch) == ESP_ERR_INVALID_STATE);
    assert(ina3221_driver_read_channels(NULL, ch) == ESP_ERR_INVALID_STATE);
    ina3221_driver_close(NULL);
}

int main(void)
{
    test_open_writes_exact_config_and_verifies();
    test_open_fails_closed_on_every_bus_error();
    test_open_rejects_config_readback_mismatch();
    test_read_channels_addressing_scaling_and_signs();
    test_read_channels_propagates_any_bus_error();
    test_close_and_guards();
    return 0;
}
