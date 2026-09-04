#include "web_status_policy.h"

web_system_state_t web_status_policy_evaluate(
    bool acquisition_running,
    bool acquisition_requested,
    bool acquisition_healthy,
    bool acquisition_error,
    size_t ready_pods,
    bool pod_error,
    size_t required_pods
)
{
    if (pod_error || (acquisition_requested && !acquisition_healthy)) {
        return WEB_SYSTEM_FAULT;
    }

    if (acquisition_error) {
        return WEB_SYSTEM_NOT_READY;
    }

    if (
        acquisition_running &&
        required_pods > 0U &&
        ready_pods >= required_pods
    ) {
        return WEB_SYSTEM_READY;
    }

    if (acquisition_running || ready_pods > 0U) {
        return WEB_SYSTEM_DEGRADED;
    }

    return WEB_SYSTEM_NOT_READY;
}

const char *web_status_policy_name(web_system_state_t state)
{
    switch (state) {
        case WEB_SYSTEM_READY:
            return "READY";
        case WEB_SYSTEM_DEGRADED:
            return "DEGRADED";
        case WEB_SYSTEM_FAULT:
            return "FAULT";
        case WEB_SYSTEM_NOT_READY:
        default:
            return "NOT_READY";
    }
}
