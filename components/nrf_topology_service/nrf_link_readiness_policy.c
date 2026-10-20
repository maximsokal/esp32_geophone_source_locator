#include "nrf_link_readiness_policy.h"

#include <limits.h>

static void schedule_retry(nrf_link_readiness_t *link, uint64_t now_ms)
{
    uint32_t delay_ms = link->retry_backoff_ms;

    if (delay_ms == 0U) {
        delay_ms = NRF_LINK_WAIT_INITIAL_BACKOFF_MS;
    }
    link->next_probe_ms = now_ms + (uint64_t)delay_ms;
    if (delay_ms < NRF_LINK_WAIT_MAX_BACKOFF_MS) {
        const uint32_t doubled = delay_ms * 2U;
        link->retry_backoff_ms = doubled > NRF_LINK_WAIT_MAX_BACKOFF_MS
            ? NRF_LINK_WAIT_MAX_BACKOFF_MS : doubled;
    }
}

void nrf_link_readiness_init(nrf_link_readiness_t *link, uint64_t now_ms)
{
    if (link == NULL) {
        return;
    }
    *link = (nrf_link_readiness_t) {
        .state = NRF_LINK_WAITING,
        .retry_backoff_ms = NRF_LINK_WAIT_INITIAL_BACKOFF_MS,
        .grace_started_ms = now_ms,
        .next_probe_ms = now_ms,
        .initialized = true,
    };
}

bool nrf_link_readiness_is_ready(const nrf_link_readiness_t *link)
{
    return link != NULL && link->initialized && link->state == NRF_LINK_READY;
}

bool nrf_link_readiness_probe_due(const nrf_link_readiness_t *link, uint64_t now_ms)
{
    return link != NULL && link->initialized && link->state != NRF_LINK_READY &&
        now_ms >= link->next_probe_ms;
}

uint32_t nrf_link_readiness_wait_ms(const nrf_link_readiness_t *link, uint64_t now_ms)
{
    if (link == NULL || !link->initialized || link->state == NRF_LINK_READY) {
        return UINT32_MAX;
    }
    if (now_ms >= link->next_probe_ms) {
        return 0U;
    }
    const uint64_t remaining_ms = link->next_probe_ms - now_ms;
    return remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms;
}

bool nrf_link_readiness_startup_grace_elapsed(
    const nrf_link_readiness_t *link,
    uint64_t now_ms)
{
    return link != NULL && link->initialized && now_ms >= link->grace_started_ms &&
        now_ms - link->grace_started_ms >= NRF_LINK_STARTUP_GRACE_MS;
}

bool nrf_link_readiness_classification_due(
    const nrf_link_readiness_t *link,
    uint64_t now_ms)
{
    return link != NULL && link->initialized &&
        link->state != NRF_LINK_READY &&
        link->consecutive_failures >= NRF_LINK_FAILURES_TO_WAITING &&
        link->failures_since_classification >= NRF_LINK_CLASSIFY_FAILURE_INTERVAL &&
        nrf_link_readiness_startup_grace_elapsed(link, now_ms);
}

void nrf_link_readiness_classification_done(nrf_link_readiness_t *link)
{
    if (link != NULL) {
        link->failures_since_classification = 0U;
    }
}

void nrf_link_readiness_record_failure(nrf_link_readiness_t *link, uint64_t now_ms)
{
    if (link == NULL) {
        return;
    }
    if (!link->initialized) {
        nrf_link_readiness_init(link, now_ms);
    }
    if (link->state == NRF_LINK_READY) {
        link->state = NRF_LINK_DEGRADED;
        link->grace_started_ms = now_ms;
        link->consecutive_failures = 0U;
        link->failures_since_classification = 0U;
        link->retry_backoff_ms = NRF_LINK_WAIT_INITIAL_BACKOFF_MS;
    }
    if (link->consecutive_failures < UINT8_MAX) {
        ++link->consecutive_failures;
    }
    if (link->failures_since_classification < UINT8_MAX) {
        ++link->failures_since_classification;
    }
    schedule_retry(link, now_ms);
    if (link->state == NRF_LINK_DEGRADED &&
        link->consecutive_failures >= NRF_LINK_FAILURES_TO_WAITING) {
        link->state = NRF_LINK_WAITING;
    }
}

void nrf_link_readiness_record_not_ready(nrf_link_readiness_t *link, uint64_t now_ms)
{
    if (link == NULL) {
        return;
    }
    if (!link->initialized) {
        nrf_link_readiness_init(link, now_ms);
    }
    link->consecutive_failures = 0U;
    link->failures_since_classification = 0U;
    if (link->state != NRF_LINK_WAITING) {
        link->state = NRF_LINK_WAITING;
        link->grace_started_ms = now_ms;
        link->retry_backoff_ms = NRF_LINK_WAIT_INITIAL_BACKOFF_MS;
    }
    schedule_retry(link, now_ms);
}

void nrf_link_readiness_record_ready(nrf_link_readiness_t *link, uint64_t now_ms)
{
    (void)now_ms;
    if (link == NULL) {
        return;
    }
    if (!link->initialized) {
        nrf_link_readiness_init(link, now_ms);
    }
    link->state = NRF_LINK_READY;
    link->consecutive_failures = 0U;
    link->failures_since_classification = 0U;
    link->retry_backoff_ms = NRF_LINK_WAIT_INITIAL_BACKOFF_MS;
    link->next_probe_ms = 0U;
    link->ever_ready = true;
}
