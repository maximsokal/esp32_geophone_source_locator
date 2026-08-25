#include "power_button_monitor_internal.h"

#include <stddef.h>


static power_button_monitor_state_t s_state = {
    .events =
        NULL,

    .task =
        NULL,

    .running =
        false,

    .initialized =
        false,
};


power_button_monitor_state_t *
power_button_monitor_state(void)
{
    return
        &s_state;
}


unsigned power_button_required_samples(
    unsigned duration_ms
)
{
    unsigned samples =
        duration_ms /
        BUTTON_POLL_MS;

    if (
        duration_ms %
            BUTTON_POLL_MS !=
        0
    ) {
        samples++;
    }

    return
        samples > 0
        ? samples
        : 1;
}
