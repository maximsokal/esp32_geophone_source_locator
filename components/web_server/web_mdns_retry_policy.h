#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t generation;
    uint32_t retry_count;
    uint64_t deadline_ms;
    bool pending;
} web_mdns_retry_policy_t;

void web_mdns_retry_policy_cancel(
    web_mdns_retry_policy_t *policy,
    uint32_t generation
);

uint32_t web_mdns_retry_policy_schedule_failure(
    web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
);

bool web_mdns_retry_policy_pending(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation
);

bool web_mdns_retry_policy_due(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
);

uint32_t web_mdns_retry_policy_next_delay_ms(
    const web_mdns_retry_policy_t *policy,
    uint32_t generation,
    uint64_t now_ms
);

void web_mdns_retry_policy_reset_success(
    web_mdns_retry_policy_t *policy,
    uint32_t generation
);
