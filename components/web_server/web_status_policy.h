#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    WEB_SYSTEM_NOT_READY = 0,
    WEB_SYSTEM_DEGRADED,
    WEB_SYSTEM_READY,
    WEB_SYSTEM_FAULT,
} web_system_state_t;

web_system_state_t web_status_policy_evaluate(
    bool acquisition_running,
    bool acquisition_requested,
    bool acquisition_healthy,
    bool acquisition_error,
    size_t ready_pods,
    bool pod_error,
    size_t required_pods
);

const char *web_status_policy_name(web_system_state_t state);
