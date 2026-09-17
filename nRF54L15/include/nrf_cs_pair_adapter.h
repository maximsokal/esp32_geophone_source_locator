#ifndef NRF_CS_PAIR_ADAPTER_H
#define NRF_CS_PAIR_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_CS_PAIR_ADAPTER_OK = 0,
    NRF_CS_PAIR_ADAPTER_COMPLETED = 1,
    NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT = -1,
    NRF_CS_PAIR_ADAPTER_ERR_BUSY = -2,
    NRF_CS_PAIR_ADAPTER_ERR_STALE = -3,
    NRF_CS_PAIR_ADAPTER_ERR_STATE = -4,
} nrf_cs_pair_adapter_result_t;

typedef enum {
    NRF_CS_PAIR_PHASE_IDLE = 0,
    NRF_CS_PAIR_PHASE_CONNECTING,
    NRF_CS_PAIR_PHASE_SECURING,
    NRF_CS_PAIR_PHASE_MTU,
    NRF_CS_PAIR_PHASE_DISCOVERING,
    NRF_CS_PAIR_PHASE_FEATURES,
    NRF_CS_PAIR_PHASE_CAPABILITIES,
    NRF_CS_PAIR_PHASE_CONFIG,
    NRF_CS_PAIR_PHASE_CS_SECURITY,
    NRF_CS_PAIR_PHASE_PROCEDURE,
    NRF_CS_PAIR_PHASE_CLEANUP,
} nrf_cs_pair_phase_t;

typedef struct {
    int (*connect)(void *context, nrf_cs_node_id_t reflector);
    int (*secure)(void *context);
    int (*exchange_mtu)(void *context);
    int (*discover_ras)(void *context);
    int (*read_features)(void *context);
    int (*configure_ranging_data)(void *context, bool realtime);
    int (*exchange_capabilities)(void *context);
    int (*create_config)(void *context);
    int (*enable_cs_security)(void *context);
    int (*start_procedure)(void *context);
    int (*cleanup)(void *context);
    void *context;
} nrf_cs_pair_adapter_ops_t;

typedef void (*nrf_cs_pair_adapter_complete_cb_t)(
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement,
    void *context);

typedef struct {
    nrf_cs_pair_adapter_ops_t ops;
    bool active;
    nrf_cs_pair_phase_t phase;
    nrf_cs_step_t step;
    nrf_cs_measurement_t terminal_measurement;
    bool terminal_pending;
    nrf_cs_pair_adapter_complete_cb_t complete_cb;
    void *complete_context;
} nrf_cs_pair_adapter_t;

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_init(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_pair_adapter_ops_t *ops);

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_execute(
    nrf_cs_pair_adapter_t *adapter,
    nrf_cs_node_id_t local_node_id,
    const nrf_cs_step_t *step,
    nrf_cs_pair_adapter_complete_cb_t complete_cb,
    void *complete_context);

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_connected(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_security_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_mtu_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_discovery_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_features_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    bool realtime);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_capabilities_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_config_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_cs_security_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_measurement_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_transport_error(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_timeout(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);
nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_cleanup_complete(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step);

bool nrf_cs_pair_adapter_is_active(const nrf_cs_pair_adapter_t *adapter);
nrf_cs_pair_phase_t nrf_cs_pair_adapter_phase(const nrf_cs_pair_adapter_t *adapter);
const nrf_cs_step_t *nrf_cs_pair_adapter_active_step(const nrf_cs_pair_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
