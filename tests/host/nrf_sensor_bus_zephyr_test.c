#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "nrf_line_sampling_stub.h"
#include "nrf_bme280_zephyr.h"
#include "nrf_bmi160_zephyr.h"
#include "nrf_remote_sensor_runtime_zephyr.h"
#include "nrf_sensor_bus_zephyr.h"

const struct device test_i2c30 = {0};
static bool s_ready;
static bool s_no_drdy;
static uint8_t s_drdy_mask;
static bool s_ignore_forced;
static int64_t s_now_ms;
static unsigned s_sleep_calls;
static bool s_present[4];
static uint8_t s_registers[4][256];
static unsigned s_transfers[4];
static unsigned s_other_transfers;
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

int64_t k_uptime_get(void) { return s_now_ms; }

int32_t k_msleep(int32_t milliseconds)
{
    assert(milliseconds > 0);
    ++s_sleep_calls;
    return 0;
}

static int exchange(const struct device *device, uint16_t address,
    const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    assert(device == &test_i2c30 && s_ready);
    const int index = sensor_index(address);
    if (index < 0) { ++s_other_transfers; return -ENXIO; }
    ++s_transfers[index];
    if (!s_present[index]) return -ENXIO;
    const uint8_t reg = write_len > 0U ? write_data[0] : 0U;
    if (address == s_fail_address && reg == s_fail_register &&
        (read_len > 0U) == s_fail_read) return s_fail_result;
    assert((size_t)reg + read_len <= sizeof(s_registers[index]));
    if (read_len > 0U) {
        if (reg == 0xF7U) s_data_read_len = read_len;
        if (reg == 0x1BU) s_registers[index][reg] = s_no_drdy ? 0U : s_drdy_mask;
        if (reg == 0xF3U && read_len == 3U) s_registers[index][0xF4U] &= ~3U;
        memcpy(read_data, &s_registers[index][reg], read_len);
    } else {
        assert(write_len == 2U);
        if (s_ignore_forced && reg == 0xF4U && write_data[1] == 0x55U) return 0;
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
    s_no_drdy = false;
    s_drdy_mask = 0xC0U;
    s_ignore_forced = false;
    s_now_ms = 0;
    s_sleep_calls = 0U;
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
    s_other_transfers = 0U;
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
    assert(runtime.bmi160.state == NRF_BMI160_RECOVER);
    assert(runtime.bme280.state == NRF_BME280_RECOVER);
    assert(runtime.core.imu_init_result == NRF_BMI160_ERR_BUS);
    assert(runtime.core.env_init_result == NRF_BME280_ERR_BUS);

    s_ready = true;
    s_now_ms = NRF_REMOTE_SENSOR_RETRY_MS;
    s_present[0] = false;
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
        NRF_REMOTE_SENSOR_PREPARE_PARTIAL);
    assert(!runtime.core.imu_ready && runtime.core.env_ready);
    const unsigned env_transfers = s_transfers[1];
    s_present[0] = true;
    s_now_ms += NRF_REMOTE_SENSOR_RETRY_MS;
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
        NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(runtime.core.imu_ready && runtime.core.env_ready);
    assert(runtime.core.imu_init_result == 0 && runtime.core.env_init_result == 0);
    assert(s_transfers[1] == env_transfers);
}

static void test_runtime_marks_only_failed_sensor_recover_and_reprobes(void)
{
    for (unsigned sensor = 0; sensor < 3; ++sensor) {
        reset_fixture();
        if (sensor == 1) s_registers[1][0xD0U] = NRF_BMP280_CHIP_ID;
        const nrf_node_state_t remote = {
            .lifecycle = NRF_NODE_LIFECYCLE_READY, .role = NRF_NODE_ROLE_REMOTE,
        };
        nrf_remote_sensor_runtime_zephyr_t runtime;
        nrf_remote_sensor_runtime_zephyr_init(&runtime);
        assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) ==
               NRF_REMOTE_SENSOR_PREPARE_OK);
        s_fail_address = sensor == 0 ? 0x68U : 0x76U;
        s_fail_register = sensor == 0 ? 0x0CU : 0xF7U;
        s_fail_read = true;
        s_fail_result = -EIO;
        for (unsigned i = 0; i < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES; ++i) {
            (void)nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime);
        }
        assert(runtime.core.imu_ready == (sensor != 0));
        assert(runtime.core.env_ready == (sensor == 0));
        assert(runtime.bmi160.state == (sensor == 0 ? NRF_BMI160_RECOVER : NRF_BMI160_MEASURING));
        assert(runtime.bme280.state == (sensor != 0 ? NRF_BME280_RECOVER : NRF_BME280_MEASURING));
        s_fail_address = 0U;
        const unsigned good_transfers = s_transfers[sensor == 0 ? 1 : 0];
        assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
        assert(s_transfers[sensor == 0 ? 1 : 0] == good_transfers);
        assert(runtime.bmi160.state == NRF_BMI160_MEASURING);
        assert(runtime.bme280.state == NRF_BME280_MEASURING);
        assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
    }
}

static void test_async_environment_result_is_fresh_and_faults_are_counted(void)
{
    reset_fixture();
    const nrf_node_state_t remote = { .lifecycle = NRF_NODE_LIFECYCLE_READY,
                                      .role = NRF_NODE_ROLE_REMOTE };
    nrf_node_state_t main_node = remote;
    main_node.role = NRF_NODE_ROLE_MAIN;
    nrf_remote_sensor_runtime_zephyr_t runtime;
    nrf_remote_sensor_runtime_zephyr_init(&runtime);
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
    const unsigned transfers = s_transfers[1];
    assert(nrf_remote_sensor_runtime_zephyr_begin_env(&main_node, &runtime, 100U) == NRF_BME280_ERR_ARGUMENT);
    assert(s_transfers[1] == transfers);
    assert(nrf_remote_sensor_runtime_zephyr_begin_env(&remote, &runtime, 100U) == NRF_BME280_PENDING);
    assert(nrf_remote_sensor_runtime_zephyr_begin_env(&remote, &runtime, 101U) == NRF_BME280_PENDING);
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_ERR_NOT_READY);
    assert(runtime.core.snapshot.sequence == 0U);
    const unsigned started = s_transfers[1];
    assert(nrf_remote_sensor_runtime_zephyr_poll_env(&runtime, 146U) == NRF_BME280_PENDING);
    assert(s_transfers[1] == started);
    assert(nrf_remote_sensor_runtime_zephyr_poll_env(&runtime, 147U) == NRF_BME280_OK);
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
    assert((runtime.core.snapshot.valid_mask & NRF_SENSOR_VALID_ENV) != 0U);
    assert(runtime.core.env_read_failures == 0U);
    assert(!runtime.env_prepared);

    assert(nrf_remote_sensor_runtime_zephyr_begin_env(&remote, &runtime, 200U) == NRF_BME280_PENDING);
    s_fail_address = 0x76U;
    s_fail_register = 0xF7U;
    s_fail_read = true;
    s_fail_result = -EIO;
    assert(nrf_remote_sensor_runtime_zephyr_poll_env(&runtime, 247U) == NRF_BME280_OK);
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert((runtime.core.snapshot.fault_mask & NRF_SENSOR_VALID_ENV) != 0U);
    assert(runtime.core.snapshot.env.pressure_pa == 0U);
    assert(runtime.core.env_read_failures == 1U && runtime.core.imu_ready);
}

static void test_periodic_imu_failures_use_runtime_recovery_counter(void)
{
    reset_fixture();
    const nrf_node_state_t remote = { .lifecycle = NRF_NODE_LIFECYCLE_READY,
                                      .role = NRF_NODE_ROLE_REMOTE };
    nrf_remote_sensor_runtime_zephyr_t runtime;
    nrf_remote_sensor_runtime_zephyr_init(&runtime);
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
    for (unsigned i = 0; i < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES - 1U; ++i) {
        nrf_remote_sensor_runtime_zephyr_note_imu_read(&runtime, true);
        assert(runtime.core.imu_ready);
    }
    nrf_remote_sensor_runtime_zephyr_note_imu_read(&runtime, false);
    assert(runtime.core.imu_read_failures == 0U);
    for (unsigned i = 0; i < NRF_REMOTE_SENSOR_REINIT_AFTER_FAILURES; ++i)
        nrf_remote_sensor_runtime_zephyr_note_imu_read(&runtime, true);
    assert(!runtime.core.imu_ready && runtime.core.env_ready);
    assert(runtime.bmi160.state == NRF_BMI160_RECOVER);
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(runtime.bmi160.state == NRF_BMI160_MEASURING);
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

static void test_automatic_diagnostics_only_probe_expected_addresses(void)
{
    reset_fixture();
    assert(nrf_sensor_bus_zephyr_scan_expected() == 2);
    assert(s_other_transfers == 0U);
    for (size_t i = 0; i < 4U; ++i) assert(s_transfers[i] == 1U);
    assert_diagnostic("sensor_scan: expected addr=0x68 id=209");
    assert_diagnostic("sensor_scan: expected addr=0x76 id=96");
    s_ready = false;
    assert(nrf_sensor_bus_zephyr_scan_expected() == -ENODEV);
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
    uint32_t bitmap[4] = {0};
    assert(nrf_sensor_bus_zephyr_scan_private_bitmap(bitmap) == 2);
    assert(bitmap[0] == 0U && bitmap[1] == 0U && bitmap[2] == 0U);
    assert(bitmap[3] == ((UINT32_C(1) << 8U) | (UINT32_C(1) << 22U)));
    assert_diagnostic("addr=0x68 ack reg00=209");

    reset_fixture();
    s_ready = false;
    assert(nrf_sensor_bus_zephyr_scan_private_bitmap(bitmap) == -ENODEV);
    assert(bitmap[0] == 0U && bitmap[1] == 0U && bitmap[2] == 0U && bitmap[3] == 0U);
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

static void test_cooperative_prepare_on_alternate_addresses(void)
{
    reset_fixture();
    s_present[0] = false;
    s_present[1] = false;
    s_present[2] = true;
    s_present[3] = true;
    s_registers[3][0x00] = 0xD1U;
    nrf_remote_sensor_runtime_zephyr_t runtime;
    nrf_remote_sensor_runtime_zephyr_init(&runtime);
    nrf_node_state_t state;
    nrf_node_state_init(&state);
    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);
    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_MAIN) == NRF_NODE_STATE_OK);
    assert(nrf_remote_sensor_runtime_zephyr_prepare_cooperative(&state, &runtime) == NRF_REMOTE_SENSOR_PREPARE_SKIPPED);
    assert(s_transfers[0] == 0U && s_transfers[1] == 0U && s_transfers[2] == 0U && s_transfers[3] == 0U);
    nrf_node_state_init(&state);
    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);
    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_REMOTE) == NRF_NODE_STATE_OK);
    for (s_now_ms = 0; s_now_ms < 150 && (!runtime.core.imu_ready || !runtime.core.env_ready); ++s_now_ms)
        (void)nrf_remote_sensor_runtime_zephyr_prepare_cooperative(&state, &runtime);
    assert(runtime.core.imu_ready && runtime.bmi160.address == 0x69U);
    assert(runtime.core.env_ready && runtime.bme280.address == 0x77U);
    assert(s_sleep_calls == 0U);
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
        assert(s_registers[1][0xF4] == 0x54U && s_registers[1][0xF2] == 0U);
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

static void drain_faults(void)
{
    nrf_sensor_bus_fault_t ignored;
    while (nrf_sensor_bus_zephyr_take_fault(0x00U, 0xFFU, &ignored)) {
    }
}

static nrf_sensor_bus_t open_fault_bus(void)
{
    reset_fixture();
    test_line_stub_reset();
    drain_faults();
    nrf_sensor_bus_t bus = {0};
    assert(nrf_sensor_bus_zephyr_init_private(&bus));
    return bus;
}

static int fail_write(const nrf_sensor_bus_t *bus, uint16_t address, uint8_t reg, int result)
{
    const uint8_t payload[2] = {reg, 0U};
    s_fail_address = address;
    s_fail_register = reg;
    s_fail_read = false;
    s_fail_result = result;
    return bus->transfer(bus->context, (uint8_t)address, payload, sizeof(payload), NULL, 0U);
}

static void test_fault_records_cause_errno_and_line_levels(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;

    test_line_stub_set_levels(false, true);
    assert(fail_write(&bus, 0x68U, 0x7EU, -ETIMEDOUT) == -ETIMEDOUT);
    assert(nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault));
    assert(fault.address == 0x68U && fault.cause == NRF_SENSOR_BUS_ERR_TIMEOUT);
    assert(fault.errno_abs == (uint8_t)ETIMEDOUT && fault.failures == 1U);
    assert(fault.lines_valid && !fault.sda_high && fault.scl_high);
    /* The sleep pinctrl state disconnects the inputs: the lines only read true after the controller is resumed. */
    assert(test_line_stub_resume_count() == 1U && test_line_stub_suspend_count() == 1U);
    assert(test_line_stub_reads_while_suspended() == 0U);
    assert(!nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault));

    test_line_stub_set_levels(true, true);
    static const struct { int result; nrf_sensor_bus_result_t cause; } causes[] = {
        {-EIO, NRF_SENSOR_BUS_ERR_IO},
        {-EFAULT, NRF_SENSOR_BUS_ERR_IO},
        {-EAGAIN, NRF_SENSOR_BUS_ERR_IO},
        {-ENXIO, NRF_SENSOR_BUS_ERR_TRANSFER},
        {-ETIMEDOUT, NRF_SENSOR_BUS_ERR_TIMEOUT},
    };
    for (size_t i = 0U; i < sizeof(causes) / sizeof(causes[0]); ++i) {
        assert(fail_write(&bus, 0x76U, 0xF4U, causes[i].result) == causes[i].result);
        assert(nrf_sensor_bus_zephyr_take_fault(0x76U, 0x77U, &fault));
        assert(fault.address == 0x76U && fault.cause == causes[i].cause);
        assert(fault.errno_abs == (uint8_t)-causes[i].result);
        assert(fault.lines_valid && fault.sda_high && fault.scl_high);
    }
}

static void test_fault_keeps_worst_sample_and_counts_failures(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;

    test_line_stub_set_levels(true, true);
    assert(fail_write(&bus, 0x76U, 0xF4U, -EIO) == -EIO);
    assert(fail_write(&bus, 0x76U, 0xF4U, -ETIMEDOUT) == -ETIMEDOUT);
    assert(fail_write(&bus, 0x76U, 0xF4U, -EIO) == -EIO); /* a later, milder failure must not hide the timeout */
    assert(nrf_sensor_bus_zephyr_take_fault(0x76U, 0x77U, &fault));
    assert(fault.cause == NRF_SENSOR_BUS_ERR_TIMEOUT && fault.errno_abs == (uint8_t)ETIMEDOUT);
    assert(fault.failures == 3U);

    /* A line held low outranks a timeout: the bus is broken, not silent. */
    assert(fail_write(&bus, 0x76U, 0xF4U, -EIO) == -EIO);
    assert(fail_write(&bus, 0x76U, 0xF4U, -ETIMEDOUT) == -ETIMEDOUT);
    test_line_stub_set_levels(true, false);
    assert(fail_write(&bus, 0x76U, 0xF4U, -EIO) == -EIO);
    test_line_stub_set_levels(true, true);
    assert(fail_write(&bus, 0x76U, 0xF4U, -ETIMEDOUT) == -ETIMEDOUT);
    assert(nrf_sensor_bus_zephyr_take_fault(0x76U, 0x77U, &fault));
    assert(fault.cause == NRF_SENSOR_BUS_ERR_IO && fault.errno_abs == (uint8_t)EIO);
    assert(fault.lines_valid && fault.sda_high && !fault.scl_high);
    assert(fault.failures == 4U);
}

static void test_fault_take_filters_by_address_range_and_clears_only_that_range(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;
    s_present[2] = true;

    assert(fail_write(&bus, 0x68U, 0x7EU, -EIO) == -EIO);
    assert(fail_write(&bus, 0x77U, 0xE0U, -EIO) == -EIO);

    assert(!nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, NULL));
    assert(!nrf_sensor_bus_zephyr_take_fault(0x69U, 0x68U, &fault));
    assert(nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault)); /* the rejected calls cleared nothing */
    assert(fault.address == 0x68U);
    assert(!nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault));
    assert(nrf_sensor_bus_zephyr_take_fault(0x76U, 0x77U, &fault));
    assert(fault.address == 0x77U);
    assert(!nrf_sensor_bus_zephyr_take_fault(0x00U, 0xFFU, &fault));
}

static void test_fault_ends_at_the_next_successful_transfer(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;

    assert(fail_write(&bus, 0x68U, 0x7EU, -EIO) == -EIO);
    s_fail_address = 0U;
    const uint8_t payload[2] = {0x7EU, 0x00U};
    assert(bus.transfer(bus.context, 0x68U, payload, sizeof(payload), NULL, 0U) == 0);
    assert(!nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault));
}

static void test_scan_helpers_do_not_record_expected_nacks(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;
    uint32_t bitmap[4];
    (void)bus;

    s_present[2] = false;
    assert(nrf_sensor_bus_zephyr_scan_address(0x77U) == 0);
    assert(nrf_sensor_bus_zephyr_scan_private_bitmap(bitmap) >= 0);
    assert(nrf_sensor_bus_zephyr_scan_expected() >= 0);
    assert(!nrf_sensor_bus_zephyr_take_fault(0x00U, 0xFFU, &fault));
    assert(test_line_stub_resume_count() == 0U);
}

static void test_fault_table_is_bounded_and_counts_what_it_drops(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;
    uint8_t byte = 0U;
    const uint32_t dropped_before = nrf_sensor_bus_zephyr_dropped_faults();

    for (uint8_t address = 0x10U; address <= 0x18U; ++address) {
        assert(bus.transfer(bus.context, address, NULL, 0U, &byte, 1U) == -ENXIO);
    }
    assert(nrf_sensor_bus_zephyr_dropped_faults() == dropped_before + 1U); /* nine addresses, eight slots */
    assert(nrf_sensor_bus_zephyr_take_fault(0x10U, 0x18U, &fault));
    assert(fault.address == 0x10U && fault.cause == NRF_SENSOR_BUS_ERR_TRANSFER); /* equal rank: lowest address */
    assert(!nrf_sensor_bus_zephyr_take_fault(0x10U, 0x18U, &fault));
    assert(bus.transfer(bus.context, 0x19U, NULL, 0U, &byte, 1U) == -ENXIO); /* a freed slot is reusable */
    assert(nrf_sensor_bus_zephyr_take_fault(0x19U, 0x19U, &fault));
    assert(nrf_sensor_bus_zephyr_dropped_faults() == dropped_before + 1U);
}

static void test_unsampled_lines_are_reported_as_unknown_not_guessed(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    nrf_sensor_bus_fault_t fault;

    test_line_stub_set_levels(true, true);
    test_line_stub_fail_resume(true);
    assert(fail_write(&bus, 0x68U, 0x7EU, -EIO) == -EIO);
    assert(nrf_sensor_bus_zephyr_take_fault(0x68U, 0x69U, &fault));
    assert(fault.cause == NRF_SENSOR_BUS_ERR_IO && fault.errno_abs == (uint8_t)EIO);
    assert(!fault.lines_valid && !fault.sda_high && !fault.scl_high);
    assert(test_line_stub_resume_count() == 0U);
    assert_diagnostic("ret=-5 cause=io sda=-1 scl=-1");
}

static void test_take_fault_word_is_the_event_argument(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    uint32_t word = 0U;

    test_line_stub_set_levels(true, false);
    assert(fail_write(&bus, 0x76U, 0xF4U, -EIO) == -EIO);
    assert(!nrf_sensor_bus_zephyr_take_fault_word(0x76U, 0x77U, NULL));
    assert(nrf_sensor_bus_zephyr_take_fault_word(0x76U, 0x77U, &word));
    /* 0x76, IO (2), errno 5, SDA high, SCL low, levels valid, one failure */
    assert(word == (UINT32_C(0x76) | (UINT32_C(2) << 8) | (UINT32_C(5) << 16) | (UINT32_C(1) << 24) |
                    (UINT32_C(1) << 26) | (UINT32_C(1) << 27)));
    assert(!nrf_sensor_bus_zephyr_take_fault_word(0x76U, 0x77U, &word));
}

static void test_console_line_carries_cause_and_line_levels(void)
{
    nrf_sensor_bus_t bus = open_fault_bus();
    char expected[160];

    test_line_stub_set_levels(false, true);
    assert(fail_write(&bus, 0x68U, 0x7EU, -ETIMEDOUT) == -ETIMEDOUT);
    const int written = snprintf(expected, sizeof(expected),
        "sensor_i2c: bus=i2c30 addr=0x68 reg=126 write=2 read=0 ret=%d cause=timeout sda=0 scl=1",
        -ETIMEDOUT);
    assert(written > 0 && (size_t)written < sizeof(expected));
    assert_diagnostic(expected);
}

static void test_freshness_failures_preserve_the_other_sensor(void)
{
    reset_fixture();
    const nrf_node_state_t remote = {
        .lifecycle = NRF_NODE_LIFECYCLE_READY, .role = NRF_NODE_ROLE_REMOTE,
    };
    nrf_remote_sensor_runtime_zephyr_t runtime;
    nrf_remote_sensor_runtime_zephyr_init(&runtime);
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
    s_no_drdy = true;
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(runtime.core.snapshot.valid_mask == NRF_SENSOR_VALID_ENV);
    assert(runtime.core.env_ready && runtime.core.imu_ready);
    s_no_drdy = false;
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
    s_ignore_forced = true;
    assert(nrf_remote_sensor_runtime_zephyr_begin_env(&remote, &runtime, 100U) == NRF_BME280_OK);
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_PARTIAL);
    assert(runtime.core.snapshot.valid_mask == NRF_SENSOR_VALID_IMU);
    assert(runtime.core.imu_ready && !runtime.core.env_ready);
    assert(!runtime.bme280.conversion_pending && runtime.bme280.state == NRF_BME280_RECOVER);
    s_ignore_forced = false;
    assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
    assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
}

static void test_drdy_wait_is_bounded_across_runtime_readers(void)
{
    const uint8_t missing[] = {0U, 0x40U, 0x80U};
    const nrf_node_state_t remote = {
        .lifecycle = NRF_NODE_LIFECYCLE_READY, .role = NRF_NODE_ROLE_REMOTE,
    };
    for (size_t i = 0U; i < sizeof(missing); ++i) {
        reset_fixture();
        nrf_remote_sensor_runtime_zephyr_t runtime;
        nrf_remote_sensor_runtime_zephyr_init(&runtime);
        assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
        assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
        s_drdy_mask = missing[i];
        nrf_imu_sample_t sample;
        for (s_now_ms = 20; s_now_ms < 300; s_now_ms += 20) {
            assert(runtime.core.sources.read_imu(runtime.core.sources.imu_context, &sample) == NRF_BMI160_PENDING);
            assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_PARTIAL);
            assert(runtime.core.imu_ready && runtime.core.snapshot.fault_mask == 0U);
        }
        assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_PARTIAL);
        assert(!runtime.core.imu_ready && runtime.core.env_ready);
        assert(runtime.bmi160.state == NRF_BMI160_RECOVER);
        assert(runtime.core.snapshot.valid_mask == NRF_SENSOR_VALID_ENV);
        s_drdy_mask = 0xC0U;
        assert(nrf_remote_sensor_runtime_zephyr_prepare(&remote, &runtime) == NRF_REMOTE_SENSOR_PREPARE_OK);
        assert(nrf_remote_sensor_runtime_zephyr_refresh(&remote, &runtime) == NRF_SENSOR_SNAPSHOT_OK);
    }
}

int main(void)
{
    test_drdy_wait_is_bounded_across_runtime_readers();
    test_freshness_failures_preserve_the_other_sensor();
    test_fault_records_cause_errno_and_line_levels();
    test_fault_keeps_worst_sample_and_counts_failures();
    test_fault_take_filters_by_address_range_and_clears_only_that_range();
    test_fault_ends_at_the_next_successful_transfer();
    test_scan_helpers_do_not_record_expected_nacks();
    test_fault_table_is_bounded_and_counts_what_it_drops();
    test_unsampled_lines_are_reported_as_unknown_not_guessed();
    test_take_fault_word_is_the_event_argument();
    test_console_line_carries_cause_and_line_levels();
    test_cooperative_prepare_on_alternate_addresses();
    test_async_environment_result_is_fresh_and_faults_are_counted();
    test_automatic_diagnostics_only_probe_expected_addresses();
    test_periodic_imu_failures_use_runtime_recovery_counter();
    test_runtime_marks_only_failed_sensor_recover_and_reprobes();
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
