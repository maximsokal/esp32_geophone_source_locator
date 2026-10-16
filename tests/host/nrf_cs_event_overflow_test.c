#include <assert.h>
#include <stdint.h>

#include "nrf_cs_event_overflow.h"

static void test_event_overflow_counter_saturates(void)
{
    uint32_t count = 0U;

    nrf_cs_event_overflow_increment(&count);
    assert(count == 1U);
    count = UINT32_MAX - 1U;
    nrf_cs_event_overflow_increment(&count);
    assert(count == UINT32_MAX);
    nrf_cs_event_overflow_increment(&count);
    assert(count == UINT32_MAX);
    nrf_cs_event_overflow_increment(NULL);
}

int main(void)
{
    test_event_overflow_counter_saturates();
    return 0;
}
