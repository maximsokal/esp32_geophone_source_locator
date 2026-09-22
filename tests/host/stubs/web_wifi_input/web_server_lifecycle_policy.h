#pragma once

#include <stdbool.h>

static inline bool web_lifecycle_command_ingress_allowed(bool shutdown_requested)
{
    return !shutdown_requested;
}

static inline bool web_lifecycle_small_body_timeout_retry_allowed(void)
{
    return true;
}
