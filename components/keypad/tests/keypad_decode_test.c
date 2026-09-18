#include <assert.h>
#include <stdio.h>

#include "keypad_decode.h"


static void test_calibrated_centers_are_keys(void)
{
    assert(keypad_decode_raw(0) == KEYPAD_KEY_LEFT);
    assert(keypad_decode_raw(550) == KEYPAD_KEY_UP);
    assert(keypad_decode_raw(1261) == KEYPAD_KEY_DOWN);
    assert(keypad_decode_raw(1949) == KEYPAD_KEY_RIGHT);
    assert(keypad_decode_raw(2873) == KEYPAD_KEY_OK);
    assert(keypad_decode_raw(4095) == KEYPAD_KEY_NONE);
}


static void test_boundary_noise_is_not_a_key(void)
{
    const int samples[] = {
        150, 207, 242, 264, 270, 276, 292, 300, 347, 387,
        877, 884, 885, 888, 889, 899, 903, 905, 906, 910, 912,
        929, 950, 951, 964, 1027, 1044,
        1449, 1605, 1607, 1657,
        2347, 2411, 2415, 2474,
        3484, 3487, 3497,
    };

    for (size_t i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        assert(keypad_decode_raw(samples[i]) == KEYPAD_KEY_NONE);
    }

    assert(keypad_decode_raw(-1) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(4096) == KEYPAD_KEY_NONE);
}


static void test_deadband_edges_are_explicit(void)
{
    assert(keypad_decode_raw(115) == KEYPAD_KEY_LEFT);
    assert(keypad_decode_raw(116) == KEYPAD_KEY_NONE);

    assert(keypad_decode_raw(434) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(435) == KEYPAD_KEY_UP);
    assert(keypad_decode_raw(745) == KEYPAD_KEY_UP);
    assert(keypad_decode_raw(746) == KEYPAD_KEY_NONE);

    assert(keypad_decode_raw(1064) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(1065) == KEYPAD_KEY_DOWN);
    assert(keypad_decode_raw(1445) == KEYPAD_KEY_DOWN);
    assert(keypad_decode_raw(1446) == KEYPAD_KEY_NONE);

    assert(keypad_decode_raw(1764) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(1765) == KEYPAD_KEY_RIGHT);
    assert(keypad_decode_raw(2251) == KEYPAD_KEY_RIGHT);
    assert(keypad_decode_raw(2252) == KEYPAD_KEY_NONE);

    assert(keypad_decode_raw(2570) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(2571) == KEYPAD_KEY_OK);
    assert(keypad_decode_raw(3324) == KEYPAD_KEY_OK);
    assert(keypad_decode_raw(3325) == KEYPAD_KEY_NONE);
}


static void test_decoder_is_total_over_extended_adc_domain(void)
{
    for (int raw = -8192; raw <= 8192; ++raw) {
        const keypad_key_t decoded = keypad_decode_raw(raw);
        const keypad_key_t retained = keypad_decode_raw_with_previous(
            raw,
            KEYPAD_KEY_OK
        );

        assert(decoded >= KEYPAD_KEY_NONE && decoded <= KEYPAD_KEY_OK);
        assert(retained >= KEYPAD_KEY_NONE && retained <= KEYPAD_KEY_OK);
    }
}


static void test_hysteresis_retains_a_pressed_key_in_the_deadband(void)
{
    assert(
        keypad_decode_raw_with_previous(550, KEYPAD_KEY_NONE) ==
        KEYPAD_KEY_UP
    );
    assert(
        keypad_decode_raw_with_previous(899, KEYPAD_KEY_UP) ==
        KEYPAD_KEY_UP
    );
    assert(
        keypad_decode_raw_with_previous(906, KEYPAD_KEY_UP) ==
        KEYPAD_KEY_UP
    );
    assert(
        keypad_decode_raw_with_previous(1607, KEYPAD_KEY_RIGHT) ==
        KEYPAD_KEY_RIGHT
    );
    assert(
        keypad_decode_raw_with_previous(2415, KEYPAD_KEY_OK) ==
        KEYPAD_KEY_OK
    );

    /* A stable value in the next calibrated range is still allowed to make a
     * real key transition, while the open/released end always returns NONE. */
    assert(
        keypad_decode_raw_with_previous(1065, KEYPAD_KEY_UP) ==
        KEYPAD_KEY_DOWN
    );
    assert(
        keypad_decode_raw_with_previous(4095, KEYPAD_KEY_OK) ==
        KEYPAD_KEY_NONE
    );
}


int main(void)
{
    test_calibrated_centers_are_keys();
    test_boundary_noise_is_not_a_key();
    test_deadband_edges_are_explicit();
    test_decoder_is_total_over_extended_adc_domain();
    test_hysteresis_retains_a_pressed_key_in_the_deadband();
    puts("keypad decoder tests passed");
    return 0;
}
