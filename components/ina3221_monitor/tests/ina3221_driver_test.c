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
static unsigned s_probe_calls;
static unsigned s_fail_access;          /* 1-based; 0 = never */
static esp_err_t s_fail_err;
static bool s_device_present;
static bool s_config_ignores_writes;    /* chip keeps its old config */
static esp_err_t s_get_handles_err;
static unsigned s_writes;
static uint8_t s_last_write_reg;
static uint8_t s_last_write_bytes[2];
static uint8_t s_limit_stuck_reg;
static uint8_t s_short_read_reg;

static void reset_chip(void)
{
    memset(s_regs, 0, sizeof(s_regs));
    s_regs[0x00] = 0x7127U; /* INA3221 power-on default config */
    s_accesses = 0U;
    s_probe_calls = 0U;
    s_fail_access = 0U;
    s_fail_err = ESP_ERR_TIMEOUT;
    s_device_present = true;
    s_config_ignores_writes = false;
    s_get_handles_err = ESP_OK;
    s_writes = 0U;
    s_limit_stuck_reg = 0U;
    s_short_read_reg = 0xFFU;
    /* Power-on default alert limits: all disabled. */
    for (uint8_t r = 0x07U; r <= 0x0CU; ++r) {
        s_regs[r] = 0x7FF8U;
    }
    s_regs[0x0F] = 0x0002U;
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

esp_err_t sensor_bus_probe_address(uint8_t address)
{
    assert(address == 0x40U);
    ++s_probe_calls;
    return s_device_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sensor_bus_probe_address_with_context(
    uint8_t address,
    const char *source,
    int channel)
{
    assert(source != NULL && strcmp(source, "ina3221") == 0);
    assert(channel == -1);
    return sensor_bus_probe_address(address);
}

esp_err_t sensor_bus_classify_transfer_error(uint8_t address, esp_err_t err)
{
    assert(address == 0x40U);
    if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return sensor_bus_probe_address(address) == ESP_ERR_NOT_FOUND
        ? ESP_ERR_NOT_FOUND
        : err;
}

esp_err_t sensor_bus_classify_transfer_error_with_context(
    uint8_t address,
    esp_err_t err,
    const char *source,
    int channel)
{
    assert(source != NULL && strcmp(source, "ina3221") == 0);
    assert(channel == -1);
    return sensor_bus_classify_transfer_error(address, err);
}

esp_err_t sensor_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data)
{
    assert(device == &s_device_token);
    assert(data_len == 2U);
    const esp_err_t err = access_result();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_device_present) {
        return ESP_ERR_INVALID_STATE; /* IDF 5.5 wrapper NACK */
    }
    data[0] = (uint8_t)(s_regs[mem_address] >> 8);
    if (mem_address == s_short_read_reg) {
        return ESP_ERR_INVALID_RESPONSE; /* adapter could not supply byte 2 */
    }
    data[1] = (uint8_t)(s_regs[mem_address] & 0xFFU);
    return ESP_OK;
}

esp_err_t sensor_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data)
{
    assert(device == &s_device_token);
    assert(data_len == 2U);
    const esp_err_t err = access_result();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_device_present) {
        return ESP_ERR_INVALID_STATE; /* IDF 5.5 wrapper NACK */
    }
    ++s_writes;
    s_last_write_reg = mem_address;
    memcpy(s_last_write_bytes, data, 2U);
    /* Writable here: configuration and the six alert limits (MAX-245). */
    assert(mem_address == 0x00U || (mem_address >= 0x07U && mem_address <= 0x0CU));
    if (s_limit_stuck_reg != 0U && mem_address == s_limit_stuck_reg) {
        return ESP_OK; /* register ignores the write */
    }
    if (!s_config_ignores_writes || mem_address != 0x00U) {
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

static void test_nack_is_classified_then_retries_probe_only_until_ack(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);

    s_device_present = false;
    const unsigned accesses_before_nack = s_accesses;
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT];
    assert(ina3221_driver_read_channels(&driver, channels) == ESP_ERR_NOT_FOUND);
    assert(s_accesses == accesses_before_nack + 1U);
    assert(s_probe_calls == 2U); /* failed read classified by short probe */
    ina3221_driver_close(&driver);

    const unsigned accesses_after_classification = s_accesses;
    assert(ina3221_driver_open(&driver) == ESP_ERR_NOT_FOUND);
    assert(ina3221_driver_open(&driver) == ESP_ERR_NOT_FOUND);
    assert(s_accesses == accesses_after_classification);
    assert(s_probe_calls == 4U);

    s_device_present = true;
    assert(ina3221_driver_open(&driver) == ESP_OK);
    assert(driver.ready);
    assert(s_probe_calls == 5U);
    assert(s_accesses == accesses_after_classification + 3U);
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

static void test_read_channels_match_independent_conversion_oracle(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);

    const int shunt_count_fixture[INA3221_POLICY_CHANNEL_COUNT] = {
        -123, 0, -45
    };
    const unsigned bus_count_fixture[INA3221_POLICY_CHANNEL_COUNT] = {
        487U, 37U, 571U
    };
    const float current_polarity[INA3221_POLICY_CHANNEL_COUNT] = {
        1.0f, -1.0f, -1.0f
    };
    for (size_t channel = 0U; channel < INA3221_POLICY_CHANNEL_COUNT; ++channel) {
        const uint8_t shunt_register = (uint8_t)(0x01U + (2U * channel));
        const uint8_t bus_register = (uint8_t)(0x02U + (2U * channel));
        s_regs[shunt_register] = shunt_counts(shunt_count_fixture[channel]);
        s_regs[bus_register] = bus_counts(bus_count_fixture[channel]);
    }

    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT];
    assert(ina3221_driver_read_channels(&driver, channels) == ESP_OK);
    for (size_t channel = 0U; channel < INA3221_POLICY_CHANNEL_COUNT; ++channel) {
        /* INA3221 LSBs from the data format: 40 uV shunt, 8 mV bus,
         * 0.1 ohm shunt; rail channels reverse the measured current sign. */
        const float expected_shunt_v = (float)shunt_count_fixture[channel] * 0.000040f;
        const float expected_bus_v = (float)bus_count_fixture[channel] * 0.008f;
        const float expected_current_a = expected_shunt_v / 0.1f * current_polarity[channel];
        assert(channels[channel].valid);
        assert(near(channels[channel].shunt_voltage_v, expected_shunt_v));
        assert(near(channels[channel].bus_voltage_v, expected_bus_v));
        assert(near(channels[channel].current_a, expected_current_a));
    }
}

static void test_short_read_invalidates_the_previous_snapshot(void)
{
    for (unsigned cycle = 0U; cycle < 32U; ++cycle) {
        reset_chip();
        ina3221_driver_t driver;
        open_ok(&driver);
        s_regs[0x01] = shunt_counts(125);
        s_regs[0x02] = bus_counts(500U);
        s_regs[0x03] = shunt_counts(-75);
        s_regs[0x04] = bus_counts(625U);
        s_regs[0x05] = shunt_counts(25);
        s_regs[0x06] = bus_counts(600U);

        ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT];
        assert(ina3221_driver_read_channels(&driver, channels) == ESP_OK);
        for (size_t channel = 0U; channel < INA3221_POLICY_CHANNEL_COUNT; ++channel) {
            assert(channels[channel].valid);
        }

        s_short_read_reg = 0x01U;
        assert(ina3221_driver_read_channels(&driver, channels) == ESP_ERR_INVALID_RESPONSE);
        for (size_t channel = 0U; channel < INA3221_POLICY_CHANNEL_COUNT; ++channel) {
            assert(!channels[channel].valid);
            assert(near(channels[channel].shunt_voltage_v, 0.0f));
            assert(near(channels[channel].bus_voltage_v, 0.0f));
            assert(near(channels[channel].current_a, 0.0f));
        }
        ina3221_driver_close(&driver);
        assert(!driver.ready);
    }
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

static void test_program_limits_writes_and_verifies_all_six(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    /* Make the AD7606 rail visibly different before programming. */
    s_regs[0x0B] = 0x1234U;
    const unsigned accesses_before = s_accesses;
    assert(ina3221_driver_program_limits(&driver, &limits) == ESP_OK);
    /* CH1 battery programmed (150 mV / 120 mV); rails disabled. */
    assert(s_regs[0x07] == (uint16_t)(3750 * 8));
    assert(s_regs[0x08] == (uint16_t)(3000 * 8));
    for (uint8_t r = 0x09U; r <= 0x0CU; ++r) {
        assert(s_regs[r] == 0x7FF8U);
    }
    /* 6 writes + 6 readbacks. */
    assert(s_accesses - accesses_before == 12U);
}

static void test_program_limits_failures(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);

    /* A limit register that does not latch the write is reported. */
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);
    s_limit_stuck_reg = 0x08U;
    s_regs[0x08] = 0x0000U;
    assert(ina3221_driver_program_limits(&driver, &limits) == ESP_ERR_INVALID_RESPONSE);

    /* Any bus error on the way out is propagated. */
    for (unsigned failing = 1U; failing <= 12U; ++failing) {
        reset_chip();
        open_ok(&driver);
        s_fail_access = s_accesses + failing;
        assert(ina3221_driver_program_limits(&driver, &limits) == ESP_ERR_TIMEOUT);
    }

    /* Invalid limits never reach the chip. */
    reset_chip();
    open_ok(&driver);
    const unsigned writes = s_writes;
    limits.channels[0].critical_a = 5.0f;
    assert(ina3221_driver_program_limits(&driver, &limits) == ESP_ERR_INVALID_ARG);
    assert(s_writes == writes);

    /* Not open. */
    ina3221_driver_close(&driver);
    ina3221_limits_default(&limits);
    assert(ina3221_driver_program_limits(&driver, &limits) == ESP_ERR_INVALID_STATE);
}

static void test_alert_flags(void)
{
    reset_chip();
    ina3221_driver_t driver;
    open_ok(&driver);
    s_regs[0x0F] = (uint16_t)(INA3221_DRIVER_MASK_CF1 | INA3221_DRIVER_MASK_WF1 | 0x0002U);
    uint16_t flags = 0U;
    assert(ina3221_driver_read_alert_flags(&driver, &flags) == ESP_OK);
    assert((flags & INA3221_DRIVER_MASK_CF1) != 0U);
    assert((flags & INA3221_DRIVER_MASK_WF1) != 0U);
    assert(ina3221_driver_read_alert_flags(&driver, NULL) == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_read_channels_match_independent_conversion_oracle();
    test_short_read_invalidates_the_previous_snapshot();
    test_open_writes_exact_config_and_verifies();
    test_open_fails_closed_on_every_bus_error();
    test_nack_is_classified_then_retries_probe_only_until_ack();
    test_open_rejects_config_readback_mismatch();
    test_read_channels_addressing_scaling_and_signs();
    test_read_channels_propagates_any_bus_error();
    test_close_and_guards();
    test_program_limits_writes_and_verifies_all_six();
    test_program_limits_failures();
    test_alert_flags();
    return 0;
}
