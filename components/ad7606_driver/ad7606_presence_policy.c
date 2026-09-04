#include "ad7606_presence_policy.h"

#include <stddef.h>

void ad7606_presence_observer_reset(
    ad7606_presence_observer_t *observer,
    int initial_level
)
{
    if (observer == NULL) {
        return;
    }

    observer->saw_low = initial_level == 0;
    observer->high_pending = false;
    observer->completed_pulses = 0;
}

void ad7606_presence_observer_observe(
    ad7606_presence_observer_t *observer,
    int level
)
{
    if (observer == NULL) {
        return;
    }

    if (level == 0) {
        if (observer->saw_low && observer->high_pending) {
            observer->completed_pulses++;
        }

        observer->saw_low = true;
        observer->high_pending = false;
        return;
    }

    if (observer->saw_low) {
        observer->high_pending = true;
    }
}

bool ad7606_presence_observer_ready(
    const ad7606_presence_observer_t *observer
)
{
    return
        observer != NULL &&
        observer->completed_pulses >= AD7606_PRESENCE_REQUIRED_PULSES;
}
