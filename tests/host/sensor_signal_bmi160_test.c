#define main bmi160_baseline_main
#include "nrf_bmi160_test.c"
#undef main

#include <math.h>
#include <stdio.h>

static unsigned s_config_index;
static uint8_t s_config_byte;

static int corrupt_config_transfer(void *context, uint8_t address,
    const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    const int result = fake_transfer(context, address, write_data, write_len, read_data, read_len);
    if (result == 0 && write_len == 1U && read_len == 4U &&
        write_data[0] == TEST_BMI160_REG_ACCEL_CONFIG) read_data[s_config_index] = s_config_byte;
    return result;
}

static void test_bmi_configuration_readback_bytes(void)
{
    const uint8_t expected[] = {0x28U, 0x03U, 0x28U, 0x03U};
    for (s_config_index = 0U; s_config_index < 4U; ++s_config_index) {
        for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
            fake_bmi160_bus_t fake;
            seed_sensor(&fake);
            nrf_sensor_bus_t bus = {.transfer = corrupt_config_transfer, .context = &fake};
            nrf_bmi160_t device;
            s_config_byte = (uint8_t)byte;
            const bool matches = byte == expected[s_config_index];
            assert(nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake) ==
                (matches ? NRF_BMI160_OK : NRF_BMI160_ERR_CONFIG));
            assert(device.initialized == matches);
        }
    }
    puts("SIGNAL_CASE BMI.CONFIG PASS bytes=256 registers=4");
}

static void test_bmi_identity_and_power_mode_bytes(void)
{
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        fake_bmi160_bus_t fake;
        seed_sensor(&fake);
        fake.regs[TEST_BMI160_REG_CHIP_ID] = (uint8_t)byte;
        nrf_sensor_bus_t bus = make_bus(&fake);
        nrf_bmi160_t device;
        const int result = nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake);
        assert(result == (byte == 0xD1U ? NRF_BMI160_OK : NRF_BMI160_ERR_CHIP_ID));
        assert(device.initialized == (byte == 0xD1U));

        seed_sensor(&fake);
        bus = make_bus(&fake);
        assert(nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake) == NRF_BMI160_OK);
        fake.regs[TEST_BMI160_REG_PMU_STATUS] = (uint8_t)byte;
        nrf_imu_sample_t sample;
        memset(&sample, 0x55, sizeof(sample));
        const bool normal = (byte & 0x3CU) == 0x14U;
        assert(nrf_bmi160_read_imu(&device, &sample) ==
            (normal ? NRF_BMI160_OK : NRF_BMI160_ERR_PMU));
        if (!normal) assert_sample_zero(&sample);
    }
    puts("SIGNAL_CASE BMI.IDENTITY_PMU PASS bytes=256");
}

static void test_bmi_failures_at_each_transfer(void)
{
    for (int operation = 1; operation <= 10; ++operation) {
        fake_bmi160_bus_t fake;
        seed_sensor(&fake);
        fake.fail_at_transfer = operation;
        nrf_sensor_bus_t bus = make_bus(&fake);
        nrf_bmi160_t device;
        assert(nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake) == NRF_BMI160_ERR_BUS);
        assert(!device.initialized);
    }
    for (int operation = 1; operation <= 4; ++operation) {
        fake_bmi160_bus_t fake;
        seed_sensor(&fake);
        nrf_sensor_bus_t bus = make_bus(&fake);
        nrf_bmi160_t device;
        assert(nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake) == NRF_BMI160_OK);
        fake.fail_at_transfer = fake.transfer_count + operation;
        nrf_imu_sample_t sample;
        memset(&sample, 0x55, sizeof(sample));
        assert(nrf_bmi160_read_imu(&device, &sample) == NRF_BMI160_ERR_BUS);
        assert_sample_zero(&sample);
    }
    puts("SIGNAL_CASE BMI.TRANSFER_FAILURES PASS init=10 read=3");
}

static void test_all_bmi_measurement_words(void)
{
    fake_bmi160_bus_t fake;
    seed_sensor(&fake);
    nrf_sensor_bus_t bus = make_bus(&fake);
    nrf_bmi160_t device;
    assert(nrf_bmi160_init(&device, &bus, 0x68U, fake_delay_ms, &fake) == NRF_BMI160_OK);
    for (int32_t value = INT16_MIN; value <= INT16_MAX; ++value) {
        for (unsigned axis = 0U; axis < 6U; ++axis) {
            put_i16_le(&fake.regs[TEST_BMI160_REG_GYRO_DATA + 2U * axis], (int16_t)value);
        }
        put_i16_le(&fake.regs[TEST_BMI160_REG_TEMPERATURE], (int16_t)value);
        fake.regs[TEST_BMI160_REG_STATUS] = 0xC0U; /* this iteration supplies a new sample */
        nrf_imu_sample_t sample;
        memset(&sample, 0x55, sizeof(sample));
        const int result = nrf_bmi160_read_imu(&device, &sample);
        if (value == INT16_MIN) {
            /* Bosch TEMPERATURE register: 0x8000 is invalid, not -41 Celsius. */
            assert(result == NRF_BMI160_ERR_MEASUREMENT);
            assert(device.initialized);
            assert_sample_zero(&sample);
            continue;
        }
        assert(result == NRF_BMI160_OK);
        for (unsigned axis = 0U; axis < 3U; ++axis) {
            assert(sample.accel_mg[axis] == (int32_t)lround((double)value * 2000.0 / 32768.0));
            assert(sample.gyro_mdps[axis] == (int32_t)lround((double)value * 250000.0 / 32768.0));
        }
        assert(sample.temperature_milli_c == 23000 + (int32_t)lround((double)value * 1000.0 / 512.0));
    }
    puts("SIGNAL_CASE BMI.WORDS PASS words=65536 axes=6");
}

int main(void)
{
    assert(bmi160_baseline_main() == 0);
    test_bmi_identity_and_power_mode_bytes();
    test_bmi_failures_at_each_transfer();
    test_bmi_configuration_readback_bytes();
    test_all_bmi_measurement_words();
    return 0;
}
