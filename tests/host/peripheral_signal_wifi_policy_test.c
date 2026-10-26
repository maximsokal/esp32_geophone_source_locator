/*
 * Wi-Fi: every disconnect reason code the radio can deliver, every failure
 * streak, and every combination of the flags that decide whether a late radio
 * event may change the externally visible network state.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "wifi_manager_event_policy.h"
#include "wifi_manager_reconnect_policy.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

static bool is_authentication_reason(unsigned reason)
{
    return reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void every_reason_code_gets_a_bounded_defined_delay(void)
{
    static const uint32_t transient_ms[] = {0U, 500U, 1000U, 2000U, 4000U, 15000U};
    unsigned persistent_reasons = 0U;

    for (unsigned reason = 0U; reason <= 255U; ++reason) {
        for (uint32_t failures = 0U; failures <= 12U; ++failures) {
            uint32_t previous_transient = 0U;
            for (uint32_t streak = 0U; streak <= 6U; ++streak) {
                const wifi_manager_reconnect_decision_t decision =
                    wifi_manager_reconnect_policy_decide((wifi_err_reason_t)reason, failures, streak, 0xABCD0000U + reason);
                const bool persistent = is_authentication_reason(reason) ||
                    (reason == WIFI_REASON_NO_AP_FOUND && streak >= 3U);

                assert(decision.reason == (wifi_err_reason_t)reason);
                assert(decision.generation == 0xABCD0000U + reason);
                assert(decision.reconnect_class ==
                       (persistent ? WIFI_MANAGER_RECONNECT_PERSISTENT : WIFI_MANAGER_RECONNECT_TRANSIENT));

                if (persistent) {
                    const uint32_t effective = reason == WIFI_REASON_NO_AP_FOUND ? streak - 2U : streak;
                    assert(decision.delay_ms == (effective <= 1U ? 30000U : 60000U));
                    ++persistent_reasons;
                } else {
                    uint32_t index = failures == 0U ? 0U : failures - 1U;
                    if (index > 5U) index = 5U;
                    assert(decision.delay_ms == transient_ms[index]);
                    /* The streak never changes a transient delay. */
                    if (streak > 0U) assert(decision.delay_ms == previous_transient);
                    previous_transient = decision.delay_ms;
                }
                assert(decision.delay_ms <= 60000U);
            }
        }
    }
    assert(persistent_reasons > 0U);

    /* The back-off never gets shorter as the failures pile up. */
    for (unsigned reason = 0U; reason <= 255U; ++reason) {
        uint32_t last = 0U;
        for (uint32_t failures = 0U; failures <= 40U; ++failures) {
            const uint32_t delay = wifi_manager_reconnect_policy_decide(
                (wifi_err_reason_t)reason, failures, 1U, 1U).delay_ms;
            if (!is_authentication_reason(reason)) assert(delay >= last);
            last = delay;
        }
    }
    CASE_PASS("WIFI.reconnect_policy_domain");
}

static void persistent_reasons_never_hammer_the_access_point(void)
{
    /* A wrong password must back off for at least 30 s from the first failure. */
    for (uint32_t streak = 0U; streak <= 1000U; ++streak) {
        assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_AUTH_FAIL, streak, streak, 1U).delay_ms >= 30000U);
        assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, streak, streak, 1U).delay_ms >= 30000U);
    }
    /* A missing AP is retried quickly first, then slowed down after 3 misses. */
    assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 2U, 2U, 1U).delay_ms <= 15000U);
    assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 3U, 3U, 1U).delay_ms == 30000U);
    assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, 4U, 4U, 1U).delay_ms == 60000U);
    assert(wifi_manager_reconnect_policy_decide(WIFI_REASON_NO_AP_FOUND, UINT32_MAX, UINT32_MAX, 1U).delay_ms == 60000U);
    CASE_PASS("WIFI.backoff_floor");
}

static bool epoch_oracle(bool started_enabled, uint32_t started, bool current_enabled, uint32_t current)
{
    return started_enabled && current_enabled && started == current;
}

static void late_radio_events_only_commit_in_their_own_epoch(void)
{
    static const uint32_t generations[] = {0U, 1U, 2U, UINT32_MAX};
    unsigned combinations = 0U;

    for (unsigned flags = 0U; flags < 32U; ++flags) {
        const bool started_enabled = (flags & 1U) != 0U;
        const bool current_enabled = (flags & 2U) != 0U;
        const bool live_a = (flags & 4U) != 0U;
        const bool live_b = (flags & 8U) != 0U;
        for (size_t s = 0U; s < sizeof(generations) / sizeof(generations[0]); ++s) {
            for (size_t c = 0U; c < sizeof(generations) / sizeof(generations[0]); ++c) {
                const bool epoch = epoch_oracle(started_enabled, generations[s], current_enabled, generations[c]);
                assert(wifi_manager_event_epoch_matches(started_enabled, generations[s], current_enabled, generations[c]) == epoch);
                assert(wifi_manager_sta_ip_observation_accept(
                           started_enabled, generations[s], live_a, live_b, current_enabled, generations[c]) ==
                       (live_a && live_b && epoch));
                assert(wifi_manager_sta_disconnect_observation_accept(
                           started_enabled, generations[s], live_a, current_enabled, generations[c]) ==
                       (!live_a && epoch));
                assert(wifi_manager_setup_client_observation_accept(
                           started_enabled, generations[s], live_a, current_enabled, generations[c]) ==
                       (live_a && epoch));
                assert(wifi_manager_setup_disconnect_observation_accept(
                           started_enabled, generations[s], live_a, current_enabled, generations[c]) ==
                       (!live_a && epoch));
                ++combinations;
            }
        }
    }
    assert(combinations == 32U * 16U);

    /* A connect and a disconnect observation can never both be accepted for the same live state. */
    for (unsigned flags = 0U; flags < 8U; ++flags) {
        const bool live = (flags & 1U) != 0U;
        const bool enabled = (flags & 2U) != 0U;
        assert(!(wifi_manager_sta_ip_observation_accept(enabled, 5U, live, true, enabled, 5U) &&
                 wifi_manager_sta_disconnect_observation_accept(enabled, 5U, live, enabled, 5U)));
        assert(!(wifi_manager_setup_client_observation_accept(enabled, 5U, live, enabled, 5U) &&
                 wifi_manager_setup_disconnect_observation_accept(enabled, 5U, live, enabled, 5U)));
    }
    CASE_PASS("WIFI.event_epoch_truth_table");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    every_reason_code_gets_a_bounded_defined_delay();
    persistent_reasons_never_hammer_the_access_point();
    late_radio_events_only_commit_in_their_own_epoch();
    return 0;
}
