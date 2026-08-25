#include "keypad_internal.h"


/*
 * KS0466 calibration measured on this device at 3.3 V:
 *
 * Released : RAW = 4095, voltage = 3131 mV
 * SW1      : RAW =    2, voltage =    1 mV
 * SW2      : RAW =  550, voltage =  476 mV
 * SW3      : RAW = 1261, voltage = 1059 mV
 * SW4      : RAW = 1949, voltage = 1624 mV
 * SW5      : RAW = 2873, voltage = 2372 mV
 *
 * Physical mapping:
 *
 *              SW2
 *               UP
 *
 *     SW1                 SW4
 *     LEFT               RIGHT
 *
 *              SW3
 *              DOWN
 *
 *                         SW5
 *                          OK
 *
 * Thresholds remain halfway between adjacent
 * measured RAW values.
 */
#define KEYPAD_SW1_MAX_RAW   275
#define KEYPAD_SW2_MAX_RAW   905
#define KEYPAD_SW3_MAX_RAW  1605
#define KEYPAD_SW4_MAX_RAW  2411
#define KEYPAD_SW5_MAX_RAW  3484


keypad_key_t keypad_decode_raw(
    int raw
)
{
    if (
        raw <=
        KEYPAD_SW1_MAX_RAW
    ) {
        return KEYPAD_KEY_LEFT;
    }

    if (
        raw <=
        KEYPAD_SW2_MAX_RAW
    ) {
        return KEYPAD_KEY_UP;
    }

    if (
        raw <=
        KEYPAD_SW3_MAX_RAW
    ) {
        return KEYPAD_KEY_DOWN;
    }

    if (
        raw <=
        KEYPAD_SW4_MAX_RAW
    ) {
        return KEYPAD_KEY_RIGHT;
    }

    if (
        raw <=
        KEYPAD_SW5_MAX_RAW
    ) {
        return KEYPAD_KEY_OK;
    }

    return KEYPAD_KEY_NONE;
}
