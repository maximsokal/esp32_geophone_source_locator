#ifndef NRF_BOOTSTRAP_RUNTIME_ZEPHYR_H
#define NRF_BOOTSTRAP_RUNTIME_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_node_state.h"

struct nrf_boot_twis_record;

#ifdef __cplusplus
extern "C" {
#endif

int nrf_bootstrap_runtime_zephyr_start(nrf_node_state_t *node_state);

typedef enum {
    NRF_BOOTSTRAP_TWIS_TRANSACTION_NONE = 0,
    NRF_BOOTSTRAP_TWIS_TRANSACTION_WRITE_RECEIVED,
    NRF_BOOTSTRAP_TWIS_TRANSACTION_READ_REQUESTED,
} nrf_bootstrap_twis_transaction_kind_t;

typedef struct {
    uint32_t transaction_count;
    uint32_t write_received_count;
    uint32_t write_received_bytes;
    uint32_t read_requested_count;
    uint32_t callback_error_count;
    bool first_transaction_seen;
    nrf_bootstrap_twis_transaction_kind_t first_transaction_kind;
    bool last_request_type_valid;
    uint8_t last_request_type;
    uint32_t last_request_type_count;
    uint32_t diagnostic_ring_dropped_total;
    uint32_t role_assignment_sequence;
    uint32_t role_assignment_node_id;
    uint8_t role_assignment_role;
} nrf_bootstrap_twis_diagnostics_t;

/* Lock-free best-effort snapshot of the target buffer callbacks. */
void nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(
    nrf_bootstrap_twis_diagnostics_t *diagnostics);
bool nrf_bootstrap_runtime_zephyr_drain_twis_record(
    struct nrf_boot_twis_record *record);
/* Called only by the upstream TWIS IRQ, the same producer as buffer callbacks. */
void nrf_bootstrap_runtime_zephyr_record_twis_hardware(
    const struct nrf_boot_twis_record *record);

#if defined(NRF_BOOTSTRAP_RUNTIME_TEST_HOOKS)
void nrf_bootstrap_runtime_zephyr_test_forget_started(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
