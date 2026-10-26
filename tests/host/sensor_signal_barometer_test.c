#define main barometer_baseline_main
#include "main_barometer_test.c"
#undef main

#include <stdio.h>

static uint8_t signal_crc(const uint8_t bytes[6])
{
    unsigned crc = 0xFFU;
    for (unsigned byte = 0U; byte < 6U; ++byte) {
        for (int bit = 7; bit >= 0; --bit) {
            const unsigned feedback = ((crc >> 7U) ^ (bytes[byte] >> bit)) & 1U;
            crc = (crc << 1U) & 0xFFU;
            if (feedback != 0U) crc ^= 0x31U;
        }
    }
    return (uint8_t)crc;
}

static void test_barometer_id_aht_status_and_crc_bytes(void)
{
    main_barometer_request_polling();
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        reset_fake();
        setup_bmp280(0x77U, (uint8_t)byte);
        main_barometer_t barometer;
        main_barometer_init(&barometer);
        (void)main_barometer_poll(&barometer, 0U);
        assert(barometer.bmp280_initialized == (byte == 0x58U || byte == 0x60U));
        main_barometer_close(&barometer);

        reset_fake();
        setup_bmp280(0x77U, BMP_ID);
        s_fake.aht_frame[0] = (uint8_t)byte;
        s_fake.aht_frame[6] = signal_crc(s_fake.aht_frame);
        main_barometer_init(&barometer);
        (void)main_barometer_poll(&barometer, 0U);
        assert(barometer.reading.aht20_valid == ((byte & 0x88U) == 0x08U));
        main_barometer_close(&barometer);

        reset_fake();
        setup_bmp280(0x77U, BMP_ID);
        const uint8_t correct = signal_crc(s_fake.aht_frame);
        s_fake.aht_frame[6] = (uint8_t)byte;
        main_barometer_init(&barometer);
        (void)main_barometer_poll(&barometer, 0U);
        assert(barometer.reading.aht20_valid == (byte == correct));
        main_barometer_close(&barometer);
    }
    puts("SIGNAL_CASE BAROMETER.ID_STATUS_CRC PASS bytes=256");
}

static void test_all_aht20_raw_codes(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    for (uint32_t raw = 0U; raw < 1048576U; ++raw) {
        s_fake.aht_frame[0] = 0x08U;
        s_fake.aht_frame[1] = (uint8_t)(raw >> 12U);
        s_fake.aht_frame[2] = (uint8_t)(raw >> 4U);
        s_fake.aht_frame[3] = (uint8_t)((raw << 4U) | (raw >> 16U));
        s_fake.aht_frame[4] = (uint8_t)(raw >> 8U);
        s_fake.aht_frame[5] = (uint8_t)raw;
        s_fake.aht_frame[6] = signal_crc(s_fake.aht_frame);
        assert(main_barometer_aht20_crc8(s_fake.aht_frame, 6U) == s_fake.aht_frame[6]);
        (void)main_barometer_poll(&barometer, (uint64_t)raw * 1000U);
        assert(barometer.reading.aht20_valid);
        assert(barometer.reading.humidity_mpct ==
            (uint32_t)(((uint64_t)raw * 100000U + 524288U) / 1048576U));
        assert(barometer.reading.aht20_temperature_mc ==
            (int32_t)(((uint64_t)raw * 200000U + 524288U) / 1048576U) - 50000);
    }
    main_barometer_close(&barometer);
    puts("SIGNAL_CASE AHT20.WORDS PASS codes=1048576 channels=2");
}

static void test_aht20_recovers_after_lost_calibration(void)
{
    reset_fake();
    setup_bmp280(0x77U, BMP_ID);
    s_fake.aht_frame[0] = 0U;
    s_fake.aht_frame[6] = signal_crc(s_fake.aht_frame);
    main_barometer_t barometer;
    main_barometer_init(&barometer);
    (void)main_barometer_poll(&barometer, 0U);
    assert(!barometer.reading.aht20_valid && !barometer.aht20_initialized);
    setup_aht20();
    s_fake.aht_status = 0U;
    (void)main_barometer_poll(&barometer, 6000U);
    assert(barometer.aht20_initialized && barometer.reading.aht20_valid);
    assert(barometer.reading.humidity_mpct == 50000U);
    main_barometer_close(&barometer);
    puts("SIGNAL_CASE AHT20.RECOVERY PASS");
}

int main(void)
{
    assert(barometer_baseline_main() == 0);
    test_barometer_id_aht_status_and_crc_bytes();
    test_aht20_recovers_after_lost_calibration();
    test_all_aht20_raw_codes();
    return 0;
}
