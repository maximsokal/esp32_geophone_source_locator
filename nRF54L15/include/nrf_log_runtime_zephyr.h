#ifndef NRF_LOG_RUNTIME_ZEPHYR_H
#define NRF_LOG_RUNTIME_ZEPHYR_H

#include "nrf_log_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
    uint32_t twis_protocol_error_total;
    uint32_t cs_ranging_failure_total;
    uint32_t watchdog_participant_stale_total;
    uint32_t role_lost_total;
} nrf_log_runtime_zephyr_led_counters_t;

int nrf_log_runtime_zephyr_init(void);
void nrf_log_runtime_zephyr_append_event(uint16_t event_id, uint32_t arg0, uint32_t arg1);
nrf_log_error_t nrf_log_runtime_zephyr_get_led_counters(
    nrf_log_runtime_zephyr_led_counters_t *counters);
nrf_log_error_t nrf_log_runtime_zephyr_build_status(
    uint32_t request_id,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length);
nrf_log_error_t nrf_log_runtime_zephyr_build_block(
    const nrf_log_read_request_t *request,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length);
uint32_t nrf_log_runtime_zephyr_max_block_build_cycles(void);

#if defined(NRF_LOG_RING_TEST_HOOKS)
nrf_log_error_t nrf_log_runtime_zephyr_test_set_next_seq(uint32_t next_seq);
#endif

#ifdef __cplusplus
}
#endif

#endif
