#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_bus_zephyr.h"

const struct device test_i2c30 = {0};
static bool s_ready;
static bool s_present[4];
static uint8_t s_registers[4][256];
static unsigned s_transfers[4];
static uint16_t s_fail_address;
static uint8_t s_fail_register;
static bool s_fail_read;
static int s_fail_result;
static char s_console[4096];
static size_t s_data_read_len; /* length of the last read of the measurement registers (0xF7) */

static int sensor_index(uint16_t address)
{
    if (address == 0x68U) return 0;
    if (address == 0x76U) return 1;
    if (address == 0x77U) return 2;
    if (address == 0x69U) return 3;
    return -1; /* nothing else is wired; only a bus scan asks */
}

bool device_is_ready(const struct device *device)
{
    assert(device == &test_i2c30);
    return s_ready;
}

int printk(const char *format, ...)
{
    const size_t used = strlen(s_console);
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(s_console + used, sizeof(s_console) - used, format, args);
    va_end(args);
    assert(written >= 0 && (size_t)written < sizeof(s_console) - used);
    return written;
}

int32_t k_msleep(int32_t milliseconds)
{
    assert(milliseconds > 0);
    return 0;
}

static int exchange(const struct device *device, uint16_t address,
    const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    assert(device == &test_i2c30 && s_ready);
    const int index = sensor_index(address);
    if (index < 0) return -ENXIO;
    ++s_transfers[index];
    if (!s_present[index]) return -ENXIO;
    const uint8_t reg = write_len > 0U ? write_data[0] : 0U;
    if (address == s_fail_address && reg == s_fail_register &&
        (read_len > 0U) == s_fail_read) return s_fail_result;
    assert((size_t)reg + read_len <= sizeof(s_registers[index]));
    if (read_len > 0U) {
        if (reg == 0xF7U) s_data_read_len = read_len;
        memcpy(read_data, &s_registers[index][reg], read_len);
    } else {
        assert(write_len == 2U);
        s_registers[index][reg] = write_data[1];
        if ((address == 0x68U || address == 0x69U) && reg == 0x7EU) {
            if (write_data[1] == 0xB6U) s_registers[index][0x03] = 0U;
            if (write_data[1] == 0x11U) s_registers[index][0x03] |= 0x10U;
            if (write_data[1] == 0x15U) s_registers[index][0x03] |= 0x04U;
        }
    }
    return 0;
}

int i2c_write_read(const struct device *device, uint16_t address,
    const void *write_data, size_t write_len, void *read_data, size_t read_len)
{
    assert(write_len == 1U && read_len > 0U);
    return exchange(device, address, write_data, write_len, read_data, read_len);
}

int i2c_write(const struct device *device, const uint8_t *data,
    uint32_t length, uint16_t address)
{
    return exchange(device, address, data, length, NULL, 0U);
}

int i2c_read(const struct device *device, uint8_t *data,
    uint32_t length, uint16_t address)
{
    return exchange(device, address, NULL, 0U, data, length);
}

static void reset_fixture(void)
{
    s_ready = true;
    memset(s_present, 0, sizeof(s_present));
    s_present[0] = true;
    s_present[1] = true;
    memset(s_registers, 0, sizeof(s_registers));
    s_registers[0][0x00] = 0xD1U;
    s_registers[1][0xD0] = 0x60U;
    s_registers[2][0xD0] = 0x60U;
    s_registers[1][0x8E] = 1U;
    s_registers[2][0x8E] = 1U;
    memset(s_transfers, 0, sizeof(s_transfers));
    s_fail_address = 0U;
    s_fail_result = 0;
    s_console[0] = '\0';
}

static void assert_diagnostic(const char *expected)
{
#if defined(CONFIG_APP_NRF_VERBOSE_LOG) && CONFIG_APP_NRF_VERBOSE_LOG
    assert(strstr(s_console, expected) != NULL);
#else
    (void)expected;
    assert(s_console[0] == '\0');
#endif
}

static void test_controller_not_ready_and_recovery(void)
{
    reset_fixture();
    s_ready = false;
    nrf_sensor_bus_t bus = {0};
    assert(!nrf_sensor_bus_zephyr_init_private(&bus));
    assert(bus.transfer == NULL && bus.context == NULL);
    assert(s_transfers[0] == 0U && s_transfers[1] == 0U && s_transfers[2] == 0U);
    assert_diagnostic("sensor_i2c: bus=i2c30 ready=0");
    s_ready = true;
    s_console[0] = '\0';
    assert(nrf_sensor_bus_zephyr_init_private(&bus));
    assert(bus.transfer != NULL && bus.context != NULL);
    assert(s_console[0] == '\0');
}

static void test_oversized_transfers_are_rejected_before_i2c(void)
{
#if SIZE_MAX > UINT32_MAX
    reset_fixture();
    nrf_sensor_bus_t bus = {0};
    uint8_t byte = 0U;
    assert(nrf_sensor_bus_zephyr_init_private(&bus));
    const size_t oversized = (size_t)UINT32_MAX + 1U;
    assert(bus.transfer(bus.context, 0x68U, &byte, oversized, NULL, 0U) == -EINVAL);
    assert(bus.transfer(bus.context, 0x68U, NULL, 0U, &byte, oversized) == -EINVAL);
    assert(bus.transfer(bus.context, 0x68U, &byte, 1U, &byte, oversized) == -EINVAL);
    assert(s_transfers[0] == 0U);
#endif
}

static void test_init_transfer_failures_report_original_errno_and_register(void)
{
    static const struct {
        uint16_t address;
        uint8_t reg;
        size_t read_len;
        int result;
    } failures[] = {
        {0x68U, 0x7EU, 0U, -EIO},
        {0x68U, 0x00U, 1U, -ENXIO},
        {0x68U, 0x03U, 1U, -ETIMEDOUT},
        {0x68U, 0x40U, 4U, -EIO},
        {0x76U, 0xD0U, 1U, -ENXIO},
        {0x76U, 0xE0U, 0U, -EIO},
        {0x76U, 0xF3U, 1U, -ETIMEDOUT},
        {0x76U, 0x88U, 26U, -EIO},
        {0x76U, 0xE1U, 7U, -ENXIO},
        {0x76U, 0xF2U, 0U, -EIO},
        {0x76U, 0xF5U, 0U, -EIO},
        {0x76U, 0xF4U, 0U, -EIO},
        {0x76U, 0xF2U, 4U, -ETIMEDOUT},
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        reset_fixture();
        s_fail_address = failures[i].address;
        s_fail_register = failures[i].reg;
        s_fail_read = failures[i].read_len > 0U;
        s_fail_result = failures[i].result;
        if (s_fail_address == 0x68U) {
            nrf_bmi160_t device = {0};
            assert(nrf_bmi160_zephyr_init_remote(&device) == NRF_BMI160_ERR_BUS);
            assert(!device.initialized);
        } else {
            nrf_bme280_t device = {0};
            assert(nrf_bme280_zephyr_init_remote(&device) == NRF_BME280_ERR_BUS);
            assert(!device.initialized);
        }
        char expected[160];
        const int written = snprintf(expected, sizeof(expected),
            "sensor_i2c: bus=i2c30 addr=0x%02x reg=%d write=%u read=%u ret=%d",
            (unsigned)failures[i].address, (int)failures[i].reg,
            failures[i].read_len > 0U ? 1U : 2U,
            (unsigned)failures[i].read_len, failures[i].result);
        assert(written > 0 && (size_t)written < sizeof(expected));
        assert_diagnostic(expected);
    }
}

static void test_real_runtime_retries_only_failed_initializers(void)
{
    reset_fixture();
    s_ready = false;
    const nrf_node_state_t remote = {
        .lifecycle = NRF_NODE_LIFECYCLE_READY,
        .role = NRF_NODE_ROLE_REMOTE,
    };
    nrf_remote_sensor_runtime_zephyr_t runtime;
    nrf_remote_sensor_runtime_zephyr_init(&runtime);
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
        NRF_REMOTE_SENSOR_PREPARE_ERR_NO_DATA);
    assert(!runtime.core.imu_ready && !runtime.core.env_ready);
    assert(runtime.core.imu_init_result == NRF_BMI160_ERR_BUS);
    assert(runtime.core.env_init_result == NRF_BME280_ERR_BUS);

    s_ready = true;
    s_present[0] = false;
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
        NRF_REMOTE_SENSOR_PREPARE_PARTIAL);
    assert(!runtime.core.imu_ready && runtime.core.env_ready);
    const unsigned env_transfers = s_transfers[1];
    s_present[0] = true;
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
        NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(runtime.core.imu_ready && runtime.core.env_ready);
    assert(runtime.core.imu_init_result == 0 && runtime.core.env_init_result == 0);
    assert(s_transfers[1] == env_transfers);
}

static void test_successful_initialization_is_quiet_and_supports_bme_secondary(void)
{
    reset_fixture();
    nrf_bmi160_t imu = {0};
    nrf_bme280_t env = {0};
    assert(nrf_bmi160_zephyr_init_remote(&imu) == NRF_BMI160_OK);
    assert(nrf_bme280_zephyr_init_remote(&env) == NRF_BME280_OK);
    assert(imu.initialized && env.initialized);
    assert(imu.address == 0x68U && imu.chip_id == 0xD1U && env.has_humidity);
    assert_diagnostic("sensor_probe: BMI160 addr=0x68 result=0 (ok) chip_id=0xd1");
    assert_diagnostic("sensor_probe: BME/BMP280 result=0 | 0x76 answers, chip_id=0x60 (BME280) | "
                      "0x77 NO ANSWER | used=BME280 (humidity)");
    s_present[1] = false;
    s_present[2] = true;
    assert(nrf_bme280_zephyr_init_remote(&env) == NRF_BME280_OK);
    assert(env.initialized && env.address == 0x77U);
}

static void test_scan_reports_who_answers_and_their_ids(void)
{
    /* The REMOTE case seen on 2026-10-27: nothing at 0x68, a part at 0x76 whose chip id is not a BME280's. */
    reset_fixture();
    s_present[0] = false;
    s_registers[1][0xD0] = 0x58U;
    assert(nrf_sensor_bus_zephyr_scan_private() == 1);
    assert_diagnostic("sensor_scan: bus=i2c30 addr=0x76 ack reg00=0 reg75=0 regD0=88");
    assert_diagnostic("sensor_scan: bus=i2c30 devices=1");
    assert(s_transfers[0] == 1U); /* 0x68 was probed once and did not answer */

    reset_fixture();
    s_present[0] = false;
    s_present[1] = false;
    assert(nrf_sensor_bus_zephyr_scan_private() == 0);
    assert_diagnostic("sensor_scan: bus=i2c30 devices=0");

    reset_fixture();
    assert(nrf_sensor_bus_zephyr_scan_private() == 2);
    assert_diagnostic("addr=0x68 ack reg00=209");

    reset_fixture();
    s_ready = false;
    assert(nrf_sensor_bus_zephyr_scan_private() == -ENODEV);
    assert(s_console[0] == '\0');
}

static void test_bmi160_is_found_on_the_alternate_address(void)
{
    /* SA0 tied high: nothing answers on 0x68, the BMI160 is on 0x69. */
    reset_fixture();
    s_present[0] = false;
    s_present[3] = true;
    s_registers[3][0x00] = 0xD1U;
    nrf_bmi160_t imu = {0};
    assert(nrf_bmi160_zephyr_init_remote(&imu) == NRF_BMI160_OK);
    assert(imu.initialized && imu.address == 0x69U && imu.chip_id == 0xD1U);
    assert_diagnostic("sensor_probe: BMI160 addr=0x68 NO ANSWER");
    assert_diagnostic("sensor_probe: BMI160 addr=0x69 result=0 (ok) chip_id=0xd1");

    /* Something answers on 0x68 but is not a BMI160: reported with its id, and 0x69 is still tried. */
    reset_fixture();
    s_registers[0][0x00] = 0x68U;
    nrf_bmi160_t other = {0};
    assert(nrf_bmi160_zephyr_init_remote(&other) == NRF_BMI160_ERR_CHIP_ID);
    assert(!other.initialized);
    assert_diagnostic("sensor_probe: BMI160 addr=0x68 result=-3 (answers, but the chip id is not a BMI160) chip_id=0x68");
    assert_diagnostic("sensor_probe: BMI160 addr=0x69 NO ANSWER");

    /* Nothing on either address. */
    reset_fixture();
    s_present[0] = false;
    nrf_bmi160_t none = {0};
    assert(nrf_bmi160_zephyr_init_remote(&none) == NRF_BMI160_ERR_BUS);
    assert_diagnostic("sensor_probe: BMI160 addr=0x68 NO ANSWER");
    assert_diagnostic("sensor_probe: BMI160 addr=0x69 NO ANSWER");
}

static void test_bmp280_is_accepted_without_humidity(void)
{
    static const uint8_t ids[] = {0x58U, 0x57U, 0x56U};
    for (size_t i = 0U; i < sizeof(ids); ++i) {
        reset_fixture();
        s_registers[1][0xD0] = ids[i];
        s_registers[1][0xFD] = 0x80U; /* humidity bytes of a BME280 frame that would be rejected */
        s_registers[1][0xFE] = 0x00U;
        s_registers[1][0xF7] = 0x50U;
        s_registers[1][0xFA] = 0x60U;
        nrf_bme280_t env = {0};
        assert(nrf_bme280_zephyr_init_remote(&env) == NRF_BME280_OK);
        assert(env.initialized && !env.has_humidity && env.address == 0x76U);
        assert(s_registers[1][0xF2] == 0U); /* no CTRL_HUM write: the register does not exist */
        assert(s_registers[1][0xF5] == 0x10U && s_registers[1][0xF4] == 0x54U);
        char expected[160];
        (void)snprintf(expected, sizeof(expected),
            "sensor_probe: BME/BMP280 result=0 | 0x76 answers, chip_id=0x%02x (BMP280) | 0x77 NO ANSWER | "
            "used=BMP280 (no humidity)", (unsigned)ids[i]);
        assert_diagnostic(expected);

        nrf_env_sample_t sample;
        assert(nrf_bme280_read_env(&env, &sample) == NRF_BME280_OK);
        assert(sample.humidity_milli_percent == NRF_ENV_HUMIDITY_UNAVAILABLE); /* not 0 % */
        assert(s_data_read_len == 6U); /* pressure and temperature only: a BMP280 has no humidity bytes */
        assert(s_registers[1][0xF4] == 0x55U && s_registers[1][0xF2] == 0U);
    }

    /* The same module on the other address. */
    reset_fixture();
    s_present[1] = false;
    s_present[2] = true;
    s_registers[2][0xD0] = 0x58U;
    nrf_bme280_t env = {0};
    assert(nrf_bme280_zephyr_init_remote(&env) == NRF_BME280_OK);
    assert(!env.has_humidity && env.address == 0x77U);
    assert_diagnostic("0x76 NO ANSWER | 0x77 answers, chip_id=0x58 (BMP280) | used=BMP280 (no humidity)");

    /* Answers, but it is neither: reported by its id so the part can be identified. */
    reset_fixture();
    s_registers[1][0xD0] = 0x55U;
    s_present[2] = false;
    nrf_bme280_t unknown = {0};
    assert(nrf_bme280_zephyr_init_remote(&unknown) == NRF_BME280_ERR_CHIP_ID);
    assert_diagnostic("result=-3 | 0x76 answers, chip_id=0x55 (unknown part) | 0x77 NO ANSWER | used=none");

    /* Both addresses answer: the first usable one wins and both are reported. */
    reset_fixture();
    s_registers[1][0xD0] = 0x60U;
    s_registers[2][0xD0] = 0x58U;
    s_present[2] = true;
    nrf_bme280_t both = {0};
    assert(nrf_bme280_zephyr_init_remote(&both) == NRF_BME280_OK);
    assert(both.address == 0x76U && both.has_humidity);
    {
        nrf_env_sample_t sample;
        s_registers[1][0xFA] = 0x60U;
        s_registers[1][0xF7] = 0x50U;
        s_registers[1][0xFD] = 0x10U;
        assert(nrf_bme280_read_env(&both, &sample) == NRF_BME280_OK);
        assert(s_data_read_len == 8U); /* a BME280 reads its humidity bytes too */
    }
    assert_diagnostic("0x76 answers, chip_id=0x60 (BME280) | 0x77 answers, chip_id=0x58 (BMP280)");
}

int main(void)
{
    test_bmi160_is_found_on_the_alternate_address();
    test_bmp280_is_accepted_without_humidity();
    test_scan_reports_who_answers_and_their_ids();
    test_oversized_transfers_are_rejected_before_i2c();
    test_controller_not_ready_and_recovery();
    test_init_transfer_failures_report_original_errno_and_register();
    test_real_runtime_retries_only_failed_initializers();
    test_successful_initialization_is_quiet_and_supports_bme_secondary();
    return 0;
}
