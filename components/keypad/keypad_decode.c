#include "keypad_decode.h"

#include <stdbool.h>

/* Midpoints from the original KS0466 calibration. */
#define KEYPAD_SW1_MAX_RAW 275
#define KEYPAD_SW2_MAX_RAW 905
#define KEYPAD_SW3_MAX_RAW 1605
#define KEYPAD_SW4_MAX_RAW 2411
#define KEYPAD_SW5_MAX_RAW 3484
#define KEYPAD_ADC_MAX_RAW 4095


static bool keypad_key_is_valid(keypad_key_t key)
{
    return key >= KEYPAD_KEY_LEFT && key <= KEYPAD_KEY_OK;
}


keypad_key_t keypad_decode_raw(int raw)
{
    if (raw < 0 || raw > KEYPAD_ADC_MAX_RAW) {
        return KEYPAD_KEY_NONE;
    }

    if (raw <= KEYPAD_SW1_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW) {
        return KEYPAD_KEY_LEFT;
    }

    if (
        raw >= KEYPAD_SW1_MAX_RAW + KEYPAD_DECODE_DEADBAND_RAW &&
        raw <= KEYPAD_SW2_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW
    ) {
        return KEYPAD_KEY_UP;
    }

    if (
        raw >= KEYPAD_SW2_MAX_RAW + KEYPAD_DECODE_DEADBAND_RAW &&
        raw <= KEYPAD_SW3_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW
    ) {
        return KEYPAD_KEY_DOWN;
    }

    if (
        raw >= KEYPAD_SW3_MAX_RAW + KEYPAD_DECODE_DEADBAND_RAW &&
        raw <= KEYPAD_SW4_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW
    ) {
        return KEYPAD_KEY_RIGHT;
    }

    if (
        raw >= KEYPAD_SW4_MAX_RAW + KEYPAD_DECODE_DEADBAND_RAW &&
        raw <= KEYPAD_SW5_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW
    ) {
        return KEYPAD_KEY_OK;
    }

    return KEYPAD_KEY_NONE;
}


keypad_key_t keypad_decode_raw_with_previous(
    int raw,
    keypad_key_t previous
)
{
    const keypad_key_t decoded = keypad_decode_raw(raw);
    if (decoded != KEYPAD_KEY_NONE) {
        return decoded;
    }

    /* Invalid samples and the released/open-circuit end of the ladder must
     * never keep a key pressed. The remaining NONE ranges are the intentional
     * deadbands between adjacent keys, where retaining a valid press prevents
     * one ADC-count wobble from becoming a release/press pair. */
    if (
        raw < 0 ||
        raw > KEYPAD_SW5_MAX_RAW - KEYPAD_DECODE_DEADBAND_RAW ||
        !keypad_key_is_valid(previous)
    ) {
        return KEYPAD_KEY_NONE;
    }

    return previous;
}
