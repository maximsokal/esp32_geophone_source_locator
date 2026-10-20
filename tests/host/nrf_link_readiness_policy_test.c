#include "nrf_link_readiness_policy.h"

#include <assert.h>
#include <stdint.h>

static void test_wait_backoff_is_bounded_and_deterministic(void)
{
    static const uint32_t delays[] = {250U, 500U, 1000U, 2000U, 2000U, 2000U};
    nrf_link_readiness_t link;
    uint64_t now_ms = 100U;

    nrf_link_readiness_init(&link, now_ms);
    assert(nrf_link_readiness_probe_due(&link, now_ms));
    for (uint8_t i = 0U; i < (uint8_t)(sizeof(delays) / sizeof(delays[0])); ++i) {
        const uint32_t delay_ms = delays[i];
        assert(nrf_link_readiness_probe_due(&link, now_ms));
        nrf_link_readiness_record_failure(&link, now_ms);
        assert(nrf_link_readiness_wait_ms(&link, now_ms) == delay_ms);
        assert(!nrf_link_readiness_probe_due(&link, now_ms + delay_ms - 1U));
        now_ms += delay_ms;
        assert(nrf_link_readiness_probe_due(&link, now_ms));
    }
}

static void test_ready_loss_and_startup_grace(void)
{
    nrf_link_readiness_t link;
    uint64_t now_ms = 5000U;

    nrf_link_readiness_init(&link, now_ms);
    nrf_link_readiness_record_ready(&link, now_ms);
    assert(nrf_link_readiness_is_ready(&link));

    nrf_link_readiness_record_failure(&link, now_ms);
    assert(link.state == NRF_LINK_DEGRADED);
    assert(!nrf_link_readiness_is_ready(&link));
    now_ms += NRF_LINK_WAIT_INITIAL_BACKOFF_MS;
    nrf_link_readiness_record_failure(&link, now_ms);
    now_ms += 500U;
    nrf_link_readiness_record_failure(&link, now_ms);
    assert(link.state == NRF_LINK_WAITING);
    assert(!nrf_link_readiness_classification_due(
        &link, link.grace_started_ms + NRF_LINK_STARTUP_GRACE_MS - 1U));

    now_ms = link.grace_started_ms + NRF_LINK_STARTUP_GRACE_MS;
    assert(nrf_link_readiness_startup_grace_elapsed(&link, now_ms));
    for (uint8_t i = 0U; i < NRF_LINK_CLASSIFY_FAILURE_INTERVAL; ++i) {
        nrf_link_readiness_record_failure(&link, now_ms);
        now_ms += NRF_LINK_WAIT_MAX_BACKOFF_MS;
    }
    assert(nrf_link_readiness_classification_due(&link, now_ms));
    nrf_link_readiness_classification_done(&link);
    assert(!nrf_link_readiness_classification_due(&link, now_ms));

    nrf_link_readiness_record_ready(&link, now_ms);
    assert(link.state == NRF_LINK_READY);
    assert(link.consecutive_failures == 0U);
    assert(nrf_link_readiness_wait_ms(&link, now_ms) == UINT32_MAX);
}

static void test_valid_booting_status_reschedules_wait(void)
{
    nrf_link_readiness_t link;
    nrf_link_readiness_init(&link, 0U);
    nrf_link_readiness_record_failure(&link, 0U);
    nrf_link_readiness_record_not_ready(&link, 250U);

    assert(link.state == NRF_LINK_WAITING);
    assert(link.consecutive_failures == 0U);
    assert(nrf_link_readiness_wait_ms(&link, 250U) == 500U);
}

int main(void)
{
    test_wait_backoff_is_bounded_and_deterministic();
    test_ready_loss_and_startup_grace();
    test_valid_booting_status_reschedules_wait();
    return 0;
}

