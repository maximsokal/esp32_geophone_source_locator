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


/* Number of configured periods lost when notifications were coalesced. */
uint64_t ad7606_acquisition_collapsed_periods(
    uint32_t notification_count
);


/* Saturating counter addition for long-running acquisition observability. */
uint64_t ad7606_acquisition_saturating_add_u64(
    uint64_t current,
    uint64_t increment
);
