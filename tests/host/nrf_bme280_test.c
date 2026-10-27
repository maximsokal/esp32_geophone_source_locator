#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_bme280.h"
#include "nrf_baro_altitude.h"

#define TEST_REG_CHIP_ID 0xD0U
#define TEST_REG_RESET 0xE0U
#define TEST_REG_TEMP_PRESS_CALIB 0x88U
#define TEST_REG_HUMIDITY_CALIB 0xE1U
#define TEST_REG_CTRL_HUM 0xF2U
#define TEST_REG_STATUS 0xF3U
#define TEST_REG_CTRL_MEAS 0xF4U
#define TEST_REG_CONFIG 0xF5U
#define TEST_REG_DATA 0xF7U

#define TEST_STATUS_IM_UPDATE 0x01U
#define TEST_STATUS_MEASURING 0x08U

typedef struct {
    uint8_t regs[2][256];
    bool respond[2];
    /* Humidity is skipped (0x8000) when CTRL_HUM oversampling is 0 at CTRL_MEAS write. */
    bool emulate_humidity_gate;
    int transfer_count;
    int fail_at_transfer;
    uint8_t address_history[64];
    size_t address_history_count;
    uint32_t delays[32];
    size_t delay_count;
} fake_bme280_bus_t;

static int address_index(uint8_t address)
{
    if (address == NRF_BME280_ADDRESS_PRIMARY) {
        return 0;
    }
    if (address == NRF_BME280_ADDRESS_SECONDARY) {
        return 1;
    }
    return -1;
}

static int fake_transfer(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    fake_bme280_bus_t *fake = (fake_bme280_bus_t *)context;
    assert(fake != NULL);

    fake->transfer_count++;
    assert(fake->address_history_count <
           (sizeof(fake->address_history) / sizeof(fake->address_history[0])));
    fake->address_history[fake->address_history_count++] = address;

    if (fake->fail_at_transfer > 0 &&
        fake->transfer_count == fake->fail_at_transfer) {
        return -1;
    }

    const int index = address_index(address);
    if (index < 0 || !fake->respond[index]) {
        return -2;
    }

    uint8_t *regs = fake->regs[index];
    if (write_len == 2U && read_len == 0U && write_data != NULL) {
        regs[write_data[0]] = write_data[1];
        if (fake->emulate_humidity_gate && write_data[0] == TEST_REG_CTRL_MEAS &&
            (regs[TEST_REG_CTRL_HUM] & 0x07U) == 0U) {
            regs[TEST_REG_DATA + 6U] = 0x80U;
            regs[TEST_REG_DATA + 7U] = 0x00U;
        }
        return 0;
    }

    if (write_len == 1U && read_len > 0U &&
        write_data != NULL && read_data != NULL) {
        const uint8_t reg = write_data[0];
        assert((size_t)reg + read_len <= 256U);
        memcpy(read_data, &regs[reg], read_len);
        return 0;
    }

    return -3;
}

static void fake_delay_ms(void *context, uint32_t delay_ms)
{
    fake_bme280_bus_t *fake = (fake_bme280_bus_t *)context;
    assert(fake != NULL);
    assert(fake->delay_count < (sizeof(fake->delays) / sizeof(fake->delays[0])));
    fake->delays[fake->delay_count++] = delay_ms;
}

static nrf_sensor_bus_t make_bus(fake_bme280_bus_t *fake)
{
    nrf_sensor_bus_t bus = {
        .transfer = fake_transfer,
        .context = fake,
    };
    return bus;
}

static void put_u16_le(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value & 0xFFU);
    target[1] = (uint8_t)(value >> 8U);
}

static void put_i16_le(uint8_t *target, int16_t value)
{
    put_u16_le(target, (uint16_t)value);
}

static void seed_calibration(uint8_t regs[256])
{
    uint8_t *tp = &regs[TEST_REG_TEMP_PRESS_CALIB];
    put_u16_le(&tp[0], 27504U);
    put_i16_le(&tp[2], 26435);
    put_i16_le(&tp[4], -1000);
    put_u16_le(&tp[6], 36477U);
    put_i16_le(&tp[8], -10685);
    put_i16_le(&tp[10], 3024);
    put_i16_le(&tp[12], 2855);
    put_i16_le(&tp[14], 140);
    put_i16_le(&tp[16], -7);
    put_i16_le(&tp[18], 15500);
    put_i16_le(&tp[20], -14600);
    put_i16_le(&tp[22], 6000);
    tp[24] = 0U;
    tp[25] = 75U;

    uint8_t *humidity = &regs[TEST_REG_HUMIDITY_CALIB];
    put_i16_le(&humidity[0], 362);
    humidity[2] = 0U;
    /* H4=315 (0x13B), H5=50 (0x032), packed per Bosch register map. */
    humidity[3] = 0x13U;
    humidity[4] = 0x2BU;
    humidity[5] = 0x03U;
    humidity[6] = 30U;
}

static void seed_sensor(fake_bme280_bus_t *fake, uint8_t address)
{
    memset(fake, 0, sizeof(*fake));
    const int index = address_index(address);
    assert(index >= 0);
    fake->respond[index] = true;
    fake->regs[index][TEST_REG_CHIP_ID] = NRF_BME280_CHIP_ID;
    seed_calibration(fake->regs[index]);
}

static void pack_measurement(
    uint8_t regs[256],
    uint32_t raw_pressure,
    uint32_t raw_temperature,
    uint16_t raw_humidity)
{
    uint8_t *data = &regs[TEST_REG_DATA];
    data[0] = (uint8_t)(raw_pressure >> 12U);
    data[1] = (uint8_t)(raw_pressure >> 4U);
    data[2] = (uint8_t)((raw_pressure & 0x0FU) << 4U);
    data[3] = (uint8_t)(raw_temperature >> 12U);
    data[4] = (uint8_t)(raw_temperature >> 4U);
    data[5] = (uint8_t)((raw_temperature & 0x0FU) << 4U);
    data[6] = (uint8_t)(raw_humidity >> 8U);
    data[7] = (uint8_t)(raw_humidity & 0xFFU);
}

static void assert_sample_zero(const nrf_env_sample_t *sample)
{
    assert(sample != NULL);
    assert(sample->temperature_milli_c == 0);
    assert(sample->pressure_pa == 0U);
    assert(sample->humidity_milli_percent == 0U);
}

static void test_init_rejects_invalid_arguments(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(NULL, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_ARGUMENT);
    assert(nrf_bme280_init(&device, NULL, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_ARGUMENT);
    assert(nrf_bme280_init(&device, &bus, NULL, &fake) ==
           NRF_BME280_ERR_ARGUMENT);
}

static void test_init_detects_primary_and_configures_forced_profile(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    assert(device.initialized);
    assert(device.address == NRF_BME280_ADDRESS_PRIMARY);
    assert(fake.address_history[0] == NRF_BME280_ADDRESS_PRIMARY);

    const uint8_t *regs = fake.regs[0];
    assert(regs[TEST_REG_RESET] == 0xB6U);
    assert(regs[TEST_REG_CTRL_HUM] == 0x01U);
    assert(regs[TEST_REG_CONFIG] == 0x10U);
    assert(regs[TEST_REG_CTRL_MEAS] == 0x54U);
    assert(fake.delay_count == 1U);
    assert(fake.delays[0] == 2U);

    assert(device.calibration.dig_t1 == 27504U);
    assert(device.calibration.dig_t2 == 26435);
    assert(device.calibration.dig_t3 == -1000);
    assert(device.calibration.dig_p1 == 36477U);
    assert(device.calibration.dig_p2 == -10685);
    assert(device.calibration.dig_p9 == 6000);
    assert(device.calibration.dig_h1 == 75U);
    assert(device.calibration.dig_h2 == 362);
    assert(device.calibration.dig_h4 == 315);
    assert(device.calibration.dig_h5 == 50);
    assert(device.calibration.dig_h6 == 30);
}

static void test_init_falls_back_to_secondary_address(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_SECONDARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    assert(device.address == NRF_BME280_ADDRESS_SECONDARY);
    assert(fake.address_history_count >= 2U);
    assert(fake.address_history[0] == NRF_BME280_ADDRESS_PRIMARY);
    assert(fake.address_history[1] == NRF_BME280_ADDRESS_SECONDARY);
}

static void test_init_distinguishes_bus_absence_from_wrong_chip(void)
{
    fake_bme280_bus_t fake;
    memset(&fake, 0, sizeof(fake));
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_BUS);

    memset(&fake, 0, sizeof(fake));
    fake.respond[0] = true;
    fake.respond[1] = true;
    fake.regs[0][TEST_REG_CHIP_ID] = 0x55U; /* neither a BME280 nor a BMP280 (0x56..0x58) */
    fake.regs[1][TEST_REG_CHIP_ID] = 0x61U;
    bus = make_bus(&fake);
    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_CHIP_ID);
    assert(!device.initialized);
    /* Both addresses were probed and what they said is kept for the report. */
    assert(device.probe[0].acked && device.probe[0].chip_id == 0x55U && device.probe[0].address == 0x76U);
    assert(device.probe[1].acked && device.probe[1].chip_id == 0x61U && device.probe[1].address == 0x77U);
}

static void test_init_has_bounded_nvm_wait(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    fake.regs[0][TEST_REG_STATUS] = TEST_STATUS_IM_UPDATE;
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_NVM);
    assert(!device.initialized);
    assert(fake.delay_count == 10U);
    assert(fake.delays[0] == 2U);
    for (size_t index = 1U; index < fake.delay_count; ++index) {
        assert(fake.delays[index] == 2U);
    }
}

static void test_init_rejects_invalid_pressure_calibration(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    fake.regs[0][TEST_REG_TEMP_PRESS_CALIB + 6U] = 0U;
    fake.regs[0][TEST_REG_TEMP_PRESS_CALIB + 7U] = 0U;
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_ERR_CALIBRATION);
    assert(!device.initialized);
}

static void test_read_requires_initialized_device_and_clears_output(void)
{
    nrf_bme280_t device = {0};
    nrf_env_sample_t sample;
    memset(&sample, 0x5A, sizeof(sample));

    assert(nrf_bme280_read_env(&device, &sample) ==
           NRF_BME280_ERR_NOT_INITIALIZED);
    assert_sample_zero(&sample);
    assert(nrf_bme280_read_env(NULL, &sample) == NRF_BME280_ERR_ARGUMENT);
    assert(nrf_bme280_read_env(&device, NULL) == NRF_BME280_ERR_ARGUMENT);
}

static void test_read_matches_bosch_integer_compensation_vector(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;
    nrf_env_sample_t sample;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    pack_measurement(fake.regs[0], 415148U, 519888U, 30000U);

    assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_OK);
    assert(fake.regs[0][TEST_REG_CTRL_MEAS] == 0x55U);
    assert(sample.temperature_milli_c == 25080);
    assert(sample.pressure_pa == 100653U);
    assert(sample.humidity_milli_percent == 54285U);
    assert(fake.delays[fake.delay_count - 1U] == 47U);
}

static void test_read_has_bounded_measurement_wait_and_no_stale_output(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;
    nrf_env_sample_t sample;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    fake.regs[0][TEST_REG_STATUS] = TEST_STATUS_MEASURING;
    memset(&sample, 0xA5, sizeof(sample));

    const size_t delays_before = fake.delay_count;
    assert(nrf_bme280_read_env(&device, &sample) ==
           NRF_BME280_ERR_MEASUREMENT);
    assert_sample_zero(&sample);
    assert(fake.delay_count == delays_before + 5U);
    assert(fake.delays[delays_before] == 47U);
    for (size_t index = delays_before + 1U; index < fake.delay_count; ++index) {
        assert(fake.delays[index] == 2U);
    }
}

static void test_read_propagates_transfer_failure_without_stale_output(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;
    nrf_env_sample_t sample;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    fake.fail_at_transfer = fake.transfer_count + 1;
    memset(&sample, 0xCC, sizeof(sample));
    assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_ERR_BUS);
    assert_sample_zero(&sample);
}

static void test_pressure_to_altitude_mm_linear_approximation(void)
{
    /* Sea level reference -> zero altitude. */
    assert(nrf_baro_pressure_to_altitude_mm(101325U) == 0);

    /* Lower pressure (higher up) -> positive altitude; higher pressure
     * (lower down) -> negative altitude. 12 Pa/m, so a 120 Pa drop is
     * exactly 10 m = 10000 mm. */
    assert(nrf_baro_pressure_to_altitude_mm(101325U - 120U) == 10000);
    assert(nrf_baro_pressure_to_altitude_mm(101325U + 120U) == -10000);

    /* The lowest representable pressure remains finite in the fixed-point
     * calculation; the highest representable pressure exercises the negative
     * saturation guard. */
    assert(nrf_baro_pressure_to_altitude_mm(0U) == 8443750);
    assert(nrf_baro_pressure_to_altitude_mm(UINT32_MAX) == INT32_MIN);

    /* Regression against the REMOTE implementation before extraction. */
    for (uint32_t pressure_pa = 30000U; pressure_pa <= 120000U; ++pressure_pa) {
        const int64_t old_altitude_mm =
            ((INT64_C(101325) - (int64_t)pressure_pa) * INT64_C(1000)) /
            INT64_C(12);
        assert(nrf_baro_pressure_to_altitude_mm(pressure_pa) ==
               (int32_t)old_altitude_mm);
    }
}

/*
 * CTRL_HUM and CONFIG are volatile: after a power loss the BME280 ACKs again
 * with humidity oversampling off, so every forced conversion skips humidity
 * (0x8000) and the read failed forever with ERR_MEASUREMENT.
 */
static void test_read_restores_humidity_setup_after_sensor_reset(void)
{
    fake_bme280_bus_t fake;
    seed_sensor(&fake, NRF_BME280_ADDRESS_PRIMARY);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bme280_t device;
    nrf_env_sample_t sample;

    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
           NRF_BME280_OK);
    pack_measurement(fake.regs[0], 415148U, 519888U, 30000U);
    fake.emulate_humidity_gate = true;

    fake.regs[0][TEST_REG_CTRL_HUM] = 0U;
    fake.regs[0][TEST_REG_CONFIG] = 0U;
    fake.regs[0][TEST_REG_CTRL_MEAS] = 0U;

    assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_OK);
    assert((fake.regs[0][TEST_REG_CTRL_HUM] & 0x07U) == 0x01U);
    assert((fake.regs[0][TEST_REG_CONFIG] & 0x1CU) == 0x10U);
    assert(sample.pressure_pa == 100653U);
}

int main(void)
{
    test_read_restores_humidity_setup_after_sensor_reset();
    test_pressure_to_altitude_mm_linear_approximation();
    test_init_rejects_invalid_arguments();
    test_init_detects_primary_and_configures_forced_profile();
    test_init_falls_back_to_secondary_address();
    test_init_distinguishes_bus_absence_from_wrong_chip();
    test_init_has_bounded_nvm_wait();
    test_init_rejects_invalid_pressure_calibration();
    test_read_requires_initialized_device_and_clears_output();
    test_read_matches_bosch_integer_compensation_vector();
    test_read_has_bounded_measurement_wait_and_no_stale_output();
    test_read_propagates_transfer_failure_without_stale_output();
    return 0;
}
