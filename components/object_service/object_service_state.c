#include "object_service_internal.h"


static object_service_state_t s_state = {
    .command_queue = NULL,
    .worker_task = NULL,
    .event_callback = NULL,
    .event_user_context = NULL,
    .busy = false,
    .initialized = false,
};


object_service_state_t *object_service_internal_state(void)
{
    return
        &s_state;
}
