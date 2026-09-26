#include "web_mdns_retry_policy.h"

#include <stddef.h>

static const uint32_t WEB_MDNS_RETRY_DELAYS_MS[] = { 1000U, 2000U, 5000U, 10000U };

void web_mdns_retry_policy_cancel(web_mdns_retry_policy_t *policy, uint32_t generation)
{
    if (policy == NULL) return;
    policy->generation = generation;
    policy->retry_count = 0U;
    policy->deadline_ms = 0U;
    policy->pending = false;
}

uint32_t web_mdns_retry_policy_schedule_failure(
    web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
)
{
    if (policy == NULL || policy->generation != generation) return 0U;
    if (policy->retry_count < UINT32_MAX) ++policy->retry_count;

    const size_t delay_count = sizeof(WEB_MDNS_RETRY_DELAYS_MS) / sizeof(WEB_MDNS_RETRY_DELAYS_MS[0]);
    const size_t delay_index = policy->retry_count < delay_count
        ? (size_t)policy->retry_count - 1U
        : delay_count - 1U;
    const uint32_t delay_ms = WEB_MDNS_RETRY_DELAYS_MS[delay_index];
    policy->deadline_ms = UINT64_MAX - now_ms < delay_ms
        ? UINT64_MAX
        : now_ms + delay_ms;
    policy->pending = true;
    return delay_ms;
}

bool web_mdns_retry_policy_pending(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation
)
{
    return policy != NULL && policy->pending && policy->generation == generation;
}

bool web_mdns_retry_policy_due(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
)
{
    return web_mdns_retry_policy_pending(policy, generation) && now_ms >= policy->deadline_ms;
}

uint32_t web_mdns_retry_policy_next_delay_ms(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
)
{
    if (!web_mdns_retry_policy_pending(policy, generation) || now_ms >= policy->deadline_ms) return 0U;
    const uint64_t remaining_ms = policy->deadline_ms - now_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
}

void web_mdns_retry_policy_reset_success(
    web_mdns_retry_policy_t *policy,
    uint32_t generation
)
{
    if (policy == NULL || policy->generation != generation) return;
    policy->retry_count = 0U;
    policy->deadline_ms = 0U;
    policy->pending = false;
}
