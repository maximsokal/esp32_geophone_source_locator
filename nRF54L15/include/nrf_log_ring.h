#ifndef NRF_LOG_RING_H
#define NRF_LOG_RING_H

#include "nrf_log_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_LOG_RING_CAPACITY 128U
#define NRF_LOG_RING_ROLLOVER_SEQ 0xFFFFFF00U

typedef uintptr_t nrf_log_lock_key_t;
typedef nrf_log_lock_key_t (*nrf_log_lock_fn)(void *context);
typedef void (*nrf_log_unlock_fn)(void *context, nrf_log_lock_key_t key);

typedef struct {
    nrf_log_lock_fn lock;
    nrf_log_unlock_fn unlock;
    void *context;
} nrf_log_lock_interface_t;

typedef uint32_t (*nrf_log_new_boot_id_fn)(void *context);

typedef struct {
    uint8_t records[NRF_LOG_RING_CAPACITY][NRF_LOG_PROTOCOL_RECORD_LENGTH];
    uint8_t head;
    uint8_t count;
    uint8_t boot_id_fallback;
    volatile uint8_t busy;
    uint32_t boot_id;
    uint32_t oldest_seq;
    uint32_t next_seq;
    uint32_t overwritten_total;
    volatile uint32_t lost_internal_total;
    nrf_log_lock_interface_t lock;
    nrf_log_new_boot_id_fn new_boot_id;
    void *new_boot_id_context;
    uint8_t cache_valid;
    uint8_t cache_request_type;
    uint8_t cache_length;
    uint8_t cache_max_records;
    uint32_t cache_request_id;
    uint32_t cache_expected_boot_id;
    uint32_t cache_cursor_seq;
    uint8_t cache_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
} nrf_log_ring_t;

nrf_log_error_t nrf_log_ring_init(
    nrf_log_ring_t *ring,
    uint32_t boot_id,
    uint8_t boot_id_fallback,
    const nrf_log_lock_interface_t *lock,
    nrf_log_new_boot_id_fn new_boot_id,
    void *new_boot_id_context);

nrf_log_error_t nrf_log_ring_append(
    nrf_log_ring_t *ring,
    uint8_t subsystem,
    uint8_t severity,
    uint16_t event_id,
    uint32_t arg0,
    uint32_t arg1,
    uint32_t uptime_ms);

nrf_log_error_t nrf_log_ring_status_snapshot(
    nrf_log_ring_t *ring,
    nrf_log_status_t *status);

nrf_log_error_t nrf_log_ring_build_status_frame(
    nrf_log_ring_t *ring,
    uint32_t request_id,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length);

nrf_log_error_t nrf_log_ring_build_block_frame(
    nrf_log_ring_t *ring,
    uint32_t request_id,
    uint32_t expected_boot_id,
    uint32_t cursor_seq,
    uint8_t max_records,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length);

#ifdef NRF_LOG_RING_TEST_HOOKS
nrf_log_error_t nrf_log_ring_test_set_next_seq(nrf_log_ring_t *ring, uint32_t next_seq);
nrf_log_error_t nrf_log_ring_test_set_totals(
    nrf_log_ring_t *ring,
    uint32_t overwritten_total,
    uint32_t lost_internal_total);
#endif

#ifdef __cplusplus
}
#endif

#endif
