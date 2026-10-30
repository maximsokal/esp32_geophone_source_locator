#define main barometer_baseline_main
#include "main_barometer_test.c"
#undef main

#include <stdio.h>

static void test_barometer_chip_id_bytes(void)
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
    }
    puts("SIGNAL_CASE BAROMETER.ID PASS bytes=256");
}

int main(void)
{
    assert(barometer_baseline_main() == 0);
    test_barometer_chip_id_bytes();
    return 0;
}
