#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "wifi_manager_reconnect_policy.h"

static void test_transient_delay_sequence_and_cap(void)
{
    static const uint32_t expected_ms[] = {
        0U, 500U, 1000U, 2000U, 4000U, 15000U, 15000U
    };

    for (uint32_t failure = 1U;
         failure <= (uint32_t)(sizeof(expected_ms) / sizeof(expected_ms[0]));
         ++failure) {
        const wifi_manager_reconnect_decision_t decision =
            wifi_manager_reconnect_policy_decide(
                WIFI_REASON_BEACON_TIMEOUT,
                failure,
                failure,
                42U
            );
        assert(decision.reconnect_class == WIFI_MANAGER_RECONNECT_TRANSIENT);
        assert(decision.reason == WIFI_REASON_BEACON_TIMEOUT);
        assert(decision.delay_ms == expected_ms[failure - 1U]);
        assert(decision.generation == 42U);
    }
}

static void test_persistent_reason_delays(void)
{
    const wifi_err_reason_t reasons[] = {
        WIFI_REASON_AUTH_FAIL,
        WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT,
        WIFI_REASON_HANDSHAKE_TIMEOUT
    };

    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        const wifi_manager_reconnect_decision_t first =
            wifi_manager_reconnect_policy_decide(reasons[i], 1U, 1U, 7U);
        const wifi_manager_reconnect_decision_t repeat =
            wifi_manager_reconnect_policy_decide(reasons[i], 2U, 2U, 7U);
        const wifi_manager_reconnect_decision_t capped =
            wifi_manager_reconnect_policy_decide(reasons[i], 3U, 3U, 7U);
        assert(first.reconnect_class == WIFI_MANAGER_RECONNECT_PERSISTENT);
        assert(first.delay_ms == 30000U);
        assert(repeat.reconnect_class == WIFI_MANAGER_RECONNECT_PERSISTENT);
        assert(repeat.delay_ms == 60000U);
        assert(capped.delay_ms == 60000U);
    }
}

static void test_no_ap_found_streak_threshold(void)
{
    const wifi_manager_reconnect_decision_t first =
        wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 1U, 1U, 3U);
    const wifi_manager_reconnect_decision_t second =
        wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 2U, 2U, 3U);
    const wifi_manager_reconnect_decision_t third =
        wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 3U, 3U, 3U);
    const wifi_manager_reconnect_decision_t repeated =
        wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 4U, 4U, 3U);
    const wifi_manager_reconnect_decision_t capped =
        wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 5U, 5U, 3U);

    assert(first.reconnect_class == WIFI_MANAGER_RECONNECT_TRANSIENT);
    assert(first.delay_ms == 0U);
    assert(second.reconnect_class == WIFI_MANAGER_RECONNECT_TRANSIENT);
    assert(second.delay_ms == 500U);
    assert(third.reconnect_class == WIFI_MANAGER_RECONNECT_PERSISTENT);
    assert(third.delay_ms == 30000U);
    assert(repeated.reconnect_class == WIFI_MANAGER_RECONNECT_PERSISTENT);
    assert(repeated.delay_ms == 60000U);
    assert(capped.delay_ms == 60000U);
}

int main(void)
{
    test_transient_delay_sequence_and_cap();
    test_persistent_reason_delays();
    test_no_ap_found_streak_threshold();
    puts("wifi manager reconnect policy tests passed");
    return 0;
}
