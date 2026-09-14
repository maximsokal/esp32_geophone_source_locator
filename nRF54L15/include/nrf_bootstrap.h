#ifndef NRF_BOOTSTRAP_H
#define NRF_BOOTSTRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_node_state.h"
#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int (*readdress)(void *context, uint8_t address);
    bool (*target_is_registered)(void *context);
    uint8_t (*target_address)(void *context);
    void (*cold_reboot)(void *context);
    void *context;
} nrf_bootstrap_ops_t;

typedef struct {
    nrf_node_state_t *node_state;
    nrf_bootstrap_ops_t ops;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t hardware_id_length;
    bool assigned;
    uint8_t node_id;
    uint8_t mux_channel;
    uint32_t status_request_id;
    nrf_upstream_result_t result;
    bool request_history_valid;
    uint32_t request_history_id;
    nrf_upstream_assign_t request_history_assign;
    nrf_upstream_result_t request_history_result;
} nrf_bootstrap_t;

nrf_upstream_result_t nrf_bootstrap_init(
    nrf_bootstrap_t *bootstrap,
    nrf_node_state_t *node_state,
    const uint8_t *hardware_id,
    size_t hardware_id_length,
    const nrf_bootstrap_ops_t *ops);

nrf_upstream_result_t nrf_bootstrap_process_assign(
    nrf_bootstrap_t *bootstrap,
    const nrf_upstream_assign_t *assign);

nrf_upstream_result_t nrf_bootstrap_make_status(
    const nrf_bootstrap_t *bootstrap,
    nrf_upstream_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
