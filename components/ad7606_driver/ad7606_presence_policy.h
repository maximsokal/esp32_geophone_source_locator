#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AD7606_PRESENCE_REQUIRED_PULSES 3U

typedef struct
{
    bool saw_low;
    bool high_pending;
    uint32_t completed_pulses;
} ad7606_presence_observer_t;

void ad7606_presence_observer_reset(
    ad7606_presence_observer_t *observer,
    int initial_level
);

void ad7606_presence_observer_observe(
    ad7606_presence_observer_t *observer,
    int level
);

bool ad7606_presence_observer_ready(
    const ad7606_presence_observer_t *observer
);
