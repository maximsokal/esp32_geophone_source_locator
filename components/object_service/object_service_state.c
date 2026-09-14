#include "object_service_internal.h"


static object_service_state_t s_state = {
    .command_queue = NULL,
    .worker_task = NULL,
    .event_lock = portMUX_INITIALIZER_UNLOCKED,
    .event_callback = NULL,
    .event_user_context = NULL,
    .event_callback_in_flight = 0U,
    .event_observer = NULL,
    .event_observer_context = NULL,
    .event_observer_in_flight = 0U,
    .busy = false,
    .initialized = false,
};


object_service_state_t *object_service_internal_state(void)
{
    return
        &s_state;
}
