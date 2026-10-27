#define main bme280_baseline_main
#include "nrf_bme280_test.c"
#undef main

#include <stdio.h>

static unsigned s_config_index;
static uint8_t s_config_byte;

static int corrupt_config_transfer(void *context, uint8_t address,
    const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len)
{
    const int result = fake_transfer(context, address, write_data, write_len, read_data, read_len);
    if (result == 0 && write_len == 1U && read_len == 4U &&
        write_data[0] == TEST_REG_CTRL_HUM) read_data[s_config_index] = s_config_byte;
    return result;
}

static void test_bme_configuration_readback_bytes(void)
{
    const unsigned indices[] = {0U, 2U, 3U};
    const uint8_t masks[] = {0x07U, 0xFFU, 0x1CU};
    const uint8_t expected[] = {0x01U, 0x54U, 0x10U};
    for (unsigned reg = 0U; reg < 3U; ++reg) {
        s_config_index = indices[reg];
        for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
            fake_bme280_bus_t fake;
            seed_sensor(&fake, 0x76U);
            nrf_sensor_bus_t bus = {.transfer = corrupt_config_transfer, .context = &fake};
            nrf_bme280_t device;
            s_config_byte = (uint8_t)byte;
            const bool matches = (byte & masks[reg]) == expected[reg];
            assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
                (matches ? NRF_BME280_OK : NRF_BME280_ERR_CONFIG));
            assert(device.initialized == matches);
        }
    }
    puts("SIGNAL_CASE BME.CONFIG PASS bytes=256 registers=3");
}

static void test_bme_identity_and_status_bytes(void)
{
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        fake_bme280_bus_t fake;
        seed_sensor(&fake, 0x76U);
        fake.regs[0][TEST_REG_CHIP_ID] = (uint8_t)byte;
        nrf_sensor_bus_t bus = make_bus(&fake);
        nrf_bme280_t device;
        /* 0x60 is a BME280; 0x56..0x58 are BMP280s (no humidity); every other id is refused. */
        const bool known = byte == 0x60U || (byte >= 0x56U && byte <= 0x58U);
        assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) ==
            (known ? NRF_BME280_OK : NRF_BME280_ERR_CHIP_ID));
        assert(!known || device.has_humidity == (byte == 0x60U));

        seed_sensor(&fake, 0x76U);
        bus = make_bus(&fake);
        assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) == NRF_BME280_OK);
        pack_measurement(fake.regs[0], 415148U, 519888U, 30000U);
        fake.regs[0][TEST_REG_STATUS] = (uint8_t)byte;
        nrf_env_sample_t sample;
        memset(&sample, 0x55, sizeof(sample));
        const bool busy = (byte & 0x09U) != 0U;
        assert(nrf_bme280_read_env(&device, &sample) ==
            (busy ? NRF_BME280_ERR_MEASUREMENT : NRF_BME280_OK));
        if (busy) assert_sample_zero(&sample);
        else assert(sample.pressure_pa == 100653U && sample.humidity_milli_percent == 54285U);
    }
    puts("SIGNAL_CASE BME.IDENTITY_STATUS PASS bytes=256");
}

static void test_bme_each_failed_transfer_and_skipped_channel(void)
{
    fake_bme280_bus_t good;
    seed_sensor(&good, 0x76U);
    nrf_sensor_bus_t bus = make_bus(&good);
    nrf_bme280_t device;
    assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &good) == NRF_BME280_OK);
    const int init_count = good.transfer_count;
    pack_measurement(good.regs[0], 415148U, 519888U, 30000U);
    nrf_env_sample_t sample;
    assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_OK);
    const int read_count = good.transfer_count - init_count;
    for (int operation = 1; operation <= init_count; ++operation) {
        fake_bme280_bus_t fake;
        seed_sensor(&fake, 0x76U);
        fake.fail_at_transfer = operation;
        bus = make_bus(&fake);
        if (operation == 2) {
            /* Transfer 2 is the probe of the other address (0x77), always made so it can be reported:
             * a second address that does not answer must not stop the sensor on 0x76. */
            assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) == NRF_BME280_OK);
            assert(device.initialized && device.address == 0x76U && !device.probe[1].acked);
            continue;
        }
        assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) != NRF_BME280_OK);
        assert(!device.initialized);
    }
    for (int operation = 1; operation <= read_count; ++operation) {
        fake_bme280_bus_t fake;
        seed_sensor(&fake, 0x76U);
        bus = make_bus(&fake);
        assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) == NRF_BME280_OK);
        pack_measurement(fake.regs[0], 415148U, 519888U, 30000U);
        fake.fail_at_transfer = fake.transfer_count + operation;
        memset(&sample, 0x55, sizeof(sample));
        assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_ERR_BUS);
        assert_sample_zero(&sample);
    }
    for (unsigned skipped = 0U; skipped < 3U; ++skipped) {
        fake_bme280_bus_t fake;
        seed_sensor(&fake, 0x76U);
        bus = make_bus(&fake);
        assert(nrf_bme280_init(&device, &bus, fake_delay_ms, &fake) == NRF_BME280_OK);
        pack_measurement(fake.regs[0], skipped == 0U ? 0x80000U : 415148U,
            skipped == 1U ? 0x80000U : 519888U, skipped == 2U ? 0x8000U : 30000U);
        memset(&sample, 0x55, sizeof(sample));
        assert(nrf_bme280_read_env(&device, &sample) == NRF_BME280_ERR_MEASUREMENT);
        assert_sample_zero(&sample);
    }
    puts("SIGNAL_CASE BME.TRANSFER_FAILURES PASS");
}

int main(void)
{
    assert(bme280_baseline_main() == 0);
    test_bme_identity_and_status_bytes();
    test_bme_each_failed_transfer_and_skipped_channel();
    test_bme_configuration_readback_bytes();
    return 0;
}
