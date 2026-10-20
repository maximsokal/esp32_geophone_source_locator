#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NRF_LINK_WAIT_INITIAL_BACKOFF_MS 250U
#define NRF_LINK_WAIT_MAX_BACKOFF_MS 2000U
#define NRF_LINK_FAILURES_TO_WAITING 3U
#define NRF_LINK_CLASSIFY_FAILURE_INTERVAL 3U
#define NRF_LINK_STARTUP_GRACE_MS 30000U
#define NRF_LINK_READ_TIMEOUT_MS 50U

typedef enum {
    NRF_LINK_WAITING = 0,
    NRF_LINK_READY,
    NRF_LINK_DEGRADED,
} nrf_link_readiness_state_t;

typedef struct {
    nrf_link_readiness_state_t state;
    uint8_t consecutive_failures;
    uint8_t failures_since_classification;
    uint32_t retry_backoff_ms;
    uint64_t grace_started_ms;
    uint64_t next_probe_ms;
    bool initialized;
    bool ever_ready;
} nrf_link_readiness_t;

void nrf_link_readiness_init(nrf_link_readiness_t *link, uint64_t now_ms);
bool nrf_link_readiness_is_ready(const nrf_link_readiness_t *link);
bool nrf_link_readiness_probe_due(const nrf_link_readiness_t *link, uint64_t now_ms);
uint32_t nrf_link_readiness_wait_ms(const nrf_link_readiness_t *link, uint64_t now_ms);
bool nrf_link_readiness_startup_grace_elapsed(
    const nrf_link_readiness_t *link,
    uint64_t now_ms);
bool nrf_link_readiness_classification_due(
    const nrf_link_readiness_t *link,
    uint64_t now_ms);
void nrf_link_readiness_classification_done(nrf_link_readiness_t *link);
void nrf_link_readiness_record_failure(nrf_link_readiness_t *link, uint64_t now_ms);
void nrf_link_readiness_record_not_ready(nrf_link_readiness_t *link, uint64_t now_ms);
void nrf_link_readiness_record_ready(nrf_link_readiness_t *link, uint64_t now_ms);

