#pragma once

#include "keypad.h"

/*
 * The KS0466 ladder has a single ADC boundary between each adjacent key.
 * Values close to a boundary are electrically ambiguous: accepting them as a
 * key turns a floating/unconnected GPIO8 into navigation input. Keep the
 * margin in raw ADC counts so the classifier remains independent of ADC
 * calibration APIs and is directly testable on the host.
 */
#define KEYPAD_DECODE_DEADBAND_RAW 160

/* A real released KS0466 ladder is near full scale. Require that baseline
 * before admitting the first press after adapter startup; this prevents a
 * floating GPIO8 that boots inside a key range from becoming a command. */
#define KEYPAD_RELEASE_MIN_RAW 3800

keypad_key_t keypad_decode_raw(int raw);

/* Retain the previous valid key while a pressed ladder signal crosses an
 * ambiguous boundary. A release above the final key range still returns NONE.
 */
keypad_key_t keypad_decode_raw_with_previous(
    int raw,
    keypad_key_t previous
);
