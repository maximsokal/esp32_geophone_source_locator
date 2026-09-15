#include "ad7606_acquisition_timing.h"

#include <limits.h>


#define AD7606_ACQUISITION_TIMEBASE_US    1000000ULL


uint64_t ad7606_acquisition_period_us(
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U) {
        return 0U;
    }

    return (
        AD7606_ACQUISITION_TIMEBASE_US +
        ((uint64_t)sample_rate_hz / 2U)
    ) /
        (uint64_t)sample_rate_hz;
}


uint64_t ad7606_acquisition_collapsed_periods(
    uint32_t notification_count
)
{
    return notification_count > 1U
        ? (uint64_t)notification_count - 1U
        : 0U;
}


uint64_t ad7606_acquisition_saturating_add_u64(
    uint64_t current,
    uint64_t increment
)
{
    if (increment > UINT64_MAX - current) {
        return UINT64_MAX;
    }

    return current + increment;
}
