#include "ad7606_acquisition_timing.h"


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
