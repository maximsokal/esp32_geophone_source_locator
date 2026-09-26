#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "web_mdns_retry_policy.h"

static void test_delay_sequence_and_cap(void)
{
    web_mdns_retry_policy_t policy = { 0 };
    static const uint32_t expected[] = { 1000U, 2000U, 5000U, 10000U, 10000U, 10000U };
    const uint32_t generation = 7U;
    uint64_t now_ms = 500U;
    web_mdns_retry_policy_cancel(&policy, generation);

    for (size_t i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const uint32_t delay_ms = web_mdns_retry_policy_schedule_failure(&policy, generation, now_ms);
        assert(delay_ms == expected[i]);
        assert(policy.retry_count == i + 1U);
        assert(web_mdns_retry_policy_pending(&policy, generation));
        assert(web_mdns_retry_policy_next_delay_ms(&policy, generation, now_ms) == delay_ms);
        assert(!web_mdns_retry_policy_due(&policy, generation, now_ms + delay_ms - 1U));
        assert(web_mdns_retry_policy_due(&policy, generation, now_ms + delay_ms));
        now_ms += delay_ms;
    }
}

static void test_success_resets_retry_count_and_deadline(void)
{
    web_mdns_retry_policy_t policy = { 0 };
    web_mdns_retry_policy_cancel(&policy, 4U);
    assert(web_mdns_retry_policy_schedule_failure(&policy, 4U, 100U) == 1000U);
    assert(web_mdns_retry_policy_schedule_failure(&policy, 4U, 1100U) == 2000U);

    web_mdns_retry_policy_reset_success(&policy, 4U);
    assert(policy.retry_count == 0U);
    assert(!web_mdns_retry_policy_pending(&policy, 4U));
    assert(web_mdns_retry_policy_next_delay_ms(&policy, 4U, 1100U) == 0U);
    assert(web_mdns_retry_policy_schedule_failure(&policy, 4U, 1200U) == 1000U);
}

static void test_state_quiesce_and_shutdown_cancel_pending_retry(void)
{
    web_mdns_retry_policy_t policy = { 0 };
    web_mdns_retry_policy_cancel(&policy, 10U);
    assert(web_mdns_retry_policy_schedule_failure(&policy, 10U, 0U) == 1000U);
    web_mdns_retry_policy_cancel(&policy, 11U); /* leaving STA_CONNECTED */
    assert(!web_mdns_retry_policy_pending(&policy, 11U));
    assert(policy.retry_count == 0U);

    assert(web_mdns_retry_policy_schedule_failure(&policy, 11U, 0U) == 1000U);
    web_mdns_retry_policy_cancel(&policy, 12U); /* quiesce */
    assert(!web_mdns_retry_policy_pending(&policy, 12U));

    assert(web_mdns_retry_policy_schedule_failure(&policy, 12U, 0U) == 1000U);
    web_mdns_retry_policy_cancel(&policy, 13U); /* shutdown */
    assert(!web_mdns_retry_policy_pending(&policy, 13U));
    assert(web_mdns_retry_policy_next_delay_ms(&policy, 13U, 0U) == 0U);
}

static void test_stale_generation_is_ignored(void)
{
    web_mdns_retry_policy_t policy = { 0 };
    web_mdns_retry_policy_cancel(&policy, 20U);
    assert(web_mdns_retry_policy_schedule_failure(&policy, 19U, 0U) == 0U);
    assert(policy.retry_count == 0U);
    assert(!web_mdns_retry_policy_pending(&policy, 19U));

    assert(web_mdns_retry_policy_schedule_failure(&policy, 20U, 100U) == 1000U);
    assert(!web_mdns_retry_policy_due(&policy, 19U, 2000U));
    web_mdns_retry_policy_reset_success(&policy, 19U);
    assert(policy.retry_count == 1U);
    assert(web_mdns_retry_policy_pending(&policy, 20U));
    assert(web_mdns_retry_policy_next_delay_ms(&policy, 20U, 200U) == 900U);
}

int main(void)
{
    test_delay_sequence_and_cap();
    test_success_resets_retry_count_and_deadline();
    test_state_quiesce_and_shutdown_cancel_pending_retry();
    test_stale_generation_is_ignored();
    puts("web_mdns_retry_policy_test: PASS");
    return 0;
}
