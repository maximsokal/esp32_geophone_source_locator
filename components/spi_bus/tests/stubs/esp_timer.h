#pragma once

#include <stdint.h>

/* Tests that care about time define SPI_BUS_TEST_CLOCK and provide the clock. */
#if defined(SPI_BUS_TEST_CLOCK)
int64_t esp_timer_get_time(void);
#else
static inline int64_t esp_timer_get_time(void)
{
    return 0;
}
#endif
