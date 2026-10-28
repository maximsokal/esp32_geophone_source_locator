#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include "nrf_sensor_upstream_runtime_zephyr.h"

const struct device test_i2c30 = {0};
static uint8_t imu[256], env[256];
static bool imu_present;
unsigned fake_sensor_bus_sleep_count, fake_sensor_bus_imu_reads;
unsigned fake_sensor_bus_imu_id_probes;
unsigned fake_sensor_bus_imu_states, fake_sensor_bus_env_states;
unsigned fake_sensor_bus_scan_reads;
uint64_t fake_sensor_bus_imu_max_gap_ms;
static uint64_t imu_last_ms;
extern void fake_sensor_bus_advance(int32_t ms);

void fake_sensor_bus_set_bad_pressure(void)
{
    env[0xF7U] = 0U; env[0xF8U] = 0U; env[0xF9U] = 0U;
}
void fake_sensor_bus_restore_pressure(void)
{ env[0xF7U] = 0x65U; env[0xF8U] = 0x5AU; env[0xF9U] = 0xC0U; }

void fake_sensor_bus_suspend_imu(void) { imu[0x03U] = 0U; }
void fake_sensor_bus_reset_env(void) { env[0xF4U] = 0U; env[0xF5U] = 0U; }
void fake_sensor_bus_hold_env_nvm(bool busy) { env[0xF3U] = busy ? 1U : 0U; }
void fake_sensor_bus_set_imu_present(bool present) { imu_present = present; }

void fake_sensor_bus_setup(void)
{
    memset(imu, 0, sizeof(imu)); memset(env, 0, sizeof(env));
    imu_present = true; fake_sensor_bus_imu_id_probes = 0U;
    imu[0] = 0xD1U; imu[0x16U] = 0x00U; imu[0x17U] = 0x40U;
    env[0xD0U] = 0x58U;
    static const uint16_t calibration[] = {
        27504U, 26435U, (uint16_t)-1000, 36477U, (uint16_t)-10685, 3024U,
        2855U, 140U, (uint16_t)-7, 15500U, (uint16_t)-14600, 6000U,
    };
    for (size_t i = 0; i < sizeof(calibration) / sizeof(calibration[0]); ++i) {
        env[0x88U + i * 2U] = (uint8_t)calibration[i];
        env[0x89U + i * 2U] = (uint8_t)(calibration[i] >> 8U);
    }
    env[0xF7U] = 0x65U; env[0xF8U] = 0x5AU; env[0xF9U] = 0xC0U;
    env[0xFAU] = 0x7EU; env[0xFBU] = 0xEDU; env[0xFCU] = 0x00U;
    fake_sensor_bus_sleep_count = 0U; fake_sensor_bus_imu_reads = 0U;
    fake_sensor_bus_imu_states = 0U; fake_sensor_bus_env_states = 0U;
    fake_sensor_bus_scan_reads = 0U;
    fake_sensor_bus_imu_max_gap_ms = 0U; imu_last_ms = 0U;
}

static void observe_live_state(uint16_t address)
{
    nrf_sensor_upstream_runtime_zephyr_diagnostics_t diagnostics;
    if (!nrf_sensor_upstream_runtime_zephyr_get_diagnostics(&diagnostics)) return;
    if (address == 0x68U && diagnostics.imu_state >= 0 && diagnostics.imu_state < 3)
        fake_sensor_bus_imu_states |= 1U << diagnostics.imu_state;
    if (address == 0x76U && diagnostics.env_state >= 0 && diagnostics.env_state < 3)
        fake_sensor_bus_env_states |= 1U << diagnostics.env_state;
}
bool device_is_ready(const struct device *device) { return device == &test_i2c30; }
int32_t k_msleep(int32_t ms) { assert(ms > 0); ++fake_sensor_bus_sleep_count; fake_sensor_bus_advance(ms); return 0; }
int i2c_write_read(const struct device *device, uint16_t address, const void *write_data,
                   size_t write_len, void *read_data, size_t read_len)
{
    assert(device == &test_i2c30 && write_len == 1U);
    observe_live_state(address);
    const uint8_t reg = *(const uint8_t *)write_data;
    if ((address == 0x68U || address == 0x69U) && reg == 0U) ++fake_sensor_bus_imu_id_probes;
    uint8_t *regs = address == 0x68U && imu_present ? imu : address == 0x76U ? env : NULL;
    if (regs == NULL) return -ENXIO;
    assert((size_t)reg + read_len <= 256U);
    if (address == 0x68U && reg == 0x0CU) {
        const uint64_t now_ms = (uint64_t)k_uptime_get();
        if (fake_sensor_bus_imu_reads != 0U && now_ms - imu_last_ms > fake_sensor_bus_imu_max_gap_ms)
            fake_sensor_bus_imu_max_gap_ms = now_ms - imu_last_ms;
        imu_last_ms = now_ms;
        ++fake_sensor_bus_imu_reads;
    }
    memcpy(read_data, &regs[reg], read_len); return 0;
}
int i2c_write(const struct device *device, const uint8_t *data, uint32_t length, uint16_t address)
{
    assert(device == &test_i2c30 && length == 2U);
    observe_live_state(address);
    uint8_t *regs = address == 0x68U && imu_present ? imu : address == 0x76U ? env : NULL;
    if (regs == NULL) return -ENXIO;
    regs[data[0]] = data[1];
    if (address == 0x68U && data[0] == 0x7EU) {
        if (data[1] == 0xB6U) regs[0x03U] = 0U;
        if (data[1] == 0x11U) regs[0x03U] |= 0x10U;
        if (data[1] == 0x15U) regs[0x03U] |= 0x04U;
    }
    return 0;
}
int i2c_read(const struct device *device, uint8_t *data, uint32_t length, uint16_t address)
{
    (void)device; (void)data; (void)length;
    ++fake_sensor_bus_scan_reads;
    return (address == 0x68U || address == 0x76U) ? 0 : -ENXIO;
}
