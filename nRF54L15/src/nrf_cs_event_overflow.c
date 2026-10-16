#include "nrf_cs_event_overflow.h"

#include <stddef.h>

void nrf_cs_event_overflow_increment(uint32_t *count)
{
    if (count != NULL && *count != UINT32_MAX) {
        ++(*count);
    }
}
