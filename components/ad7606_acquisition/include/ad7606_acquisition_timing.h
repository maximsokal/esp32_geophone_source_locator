#pragma once

#include <stdint.h>


/*
 * esp_timer accepts an integral microsecond period. Round to the nearest
 * microsecond so a configured rate does not acquire a systematic positive
 * frequency error from integer truncation.
 *
 * A zero rate is invalid and returns zero instead of dividing by zero.
 */
uint64_t ad7606_acquisition_period_us(
    uint32_t sample_rate_hz
);
