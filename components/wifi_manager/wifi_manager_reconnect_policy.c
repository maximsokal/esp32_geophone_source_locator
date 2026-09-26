#include "wifi_manager_reconnect_policy.h"

#include <stdbool.h>
#include <stddef.h>

static const uint32_t s_transient_delays_ms[] = {
    0U, 500U, 1000U, 2000U, 4000U, 15000U
};

wifi_manager_reconnect_decision_t wifi_manager_reconnect_policy_decide(
    wifi_err_reason_t reason,
    uint32_t consecutive_failure_count,
    uint32_t same_reason_streak,
    uint32_t generation
)
{
    const bool persistent_reason =
        reason == WIFI_REASON_AUTH_FAIL ||
        reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
        reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
        (reason == WIFI_REASON_NO_AP_FOUND && same_reason_streak >= 3U);

    if (persistent_reason) {
        const uint32_t persistent_streak =
            reason == WIFI_REASON_NO_AP_FOUND
                ? same_reason_streak - 2U
                : same_reason_streak;
        return (wifi_manager_reconnect_decision_t) {
            .delay_ms = persistent_streak <= 1U ? 30000U : 60000U,
            .reconnect_class = WIFI_MANAGER_RECONNECT_PERSISTENT,
            .reason = reason,
            .generation = generation,
        };
    }

    size_t delay_index = consecutive_failure_count == 0U
        ? 0U
        : (size_t)(consecutive_failure_count - 1U);
    const size_t delay_count =
        sizeof(s_transient_delays_ms) / sizeof(s_transient_delays_ms[0]);
    if (delay_index >= delay_count) {
        delay_index = delay_count - 1U;
    }

    return (wifi_manager_reconnect_decision_t) {
        .delay_ms = s_transient_delays_ms[delay_index],
        .reconnect_class = WIFI_MANAGER_RECONNECT_TRANSIENT,
        .reason = reason,
        .generation = generation,
    };
}
