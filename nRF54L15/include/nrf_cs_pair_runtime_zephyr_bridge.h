#ifndef NRF_CS_PAIR_RUNTIME_ZEPHYR_BRIDGE_H
#define NRF_CS_PAIR_RUNTIME_ZEPHYR_BRIDGE_H

#include <stdbool.h>

#include "nrf_cs_pair_adapter.h"

typedef enum {
    NRF_CS_PAIR_BRIDGE_CONNECTED,
    NRF_CS_PAIR_BRIDGE_SECURITY_READY,
    NRF_CS_PAIR_BRIDGE_MTU_READY,
    NRF_CS_PAIR_BRIDGE_DISCOVERY_READY,
    NRF_CS_PAIR_BRIDGE_FEATURES_READY,
    NRF_CS_PAIR_BRIDGE_CAPABILITIES_READY,
    NRF_CS_PAIR_BRIDGE_CONFIG_READY,
    NRF_CS_PAIR_BRIDGE_CS_SECURITY_READY,
    NRF_CS_PAIR_BRIDGE_TRANSPORT_ERROR,
    NRF_CS_PAIR_BRIDGE_DISCONNECTED,
    NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY,
    NRF_CS_PAIR_BRIDGE_TIMEOUT,
    NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE,
} nrf_cs_pair_runtime_bridge_event_t;

static inline bool nrf_cs_pair_runtime_bridge_step_matches(
    const nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    const nrf_cs_step_t *active = nrf_cs_pair_adapter_active_step(adapter);

    return step != NULL && active != NULL &&
           step->cycle_id == active->cycle_id &&
           step->membership_generation == active->membership_generation &&
           step->attempt_id == active->attempt_id &&
           step->pair_index == active->pair_index &&
           step->attempt_number == active->attempt_number &&
           step->initiator == active->initiator &&
           step->reflector == active->reflector;
}

static inline nrf_cs_pair_adapter_result_t nrf_cs_pair_runtime_bridge_dispatch(
    nrf_cs_pair_adapter_t *adapter,
    nrf_cs_pair_runtime_bridge_event_t event,
    const nrf_cs_step_t *step,
    bool realtime,
    const nrf_cs_measurement_t *measurement)
{
    switch (event) {
    case NRF_CS_PAIR_BRIDGE_CONNECTED:
        return nrf_cs_pair_adapter_connected(adapter, step);
    case NRF_CS_PAIR_BRIDGE_SECURITY_READY:
        return nrf_cs_pair_adapter_security_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_MTU_READY:
        return nrf_cs_pair_adapter_mtu_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_DISCOVERY_READY:
        return nrf_cs_pair_adapter_discovery_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_FEATURES_READY:
        return nrf_cs_pair_adapter_features_ready(adapter, step, realtime);
    case NRF_CS_PAIR_BRIDGE_CAPABILITIES_READY:
        return nrf_cs_pair_adapter_capabilities_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_CONFIG_READY:
        return nrf_cs_pair_adapter_config_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_CS_SECURITY_READY:
        return nrf_cs_pair_adapter_cs_security_ready(adapter, step);
    case NRF_CS_PAIR_BRIDGE_TRANSPORT_ERROR:
        return nrf_cs_pair_adapter_transport_error(adapter, step);
    case NRF_CS_PAIR_BRIDGE_DISCONNECTED:
        return nrf_cs_pair_adapter_phase(adapter) == NRF_CS_PAIR_PHASE_CLEANUP ?
            nrf_cs_pair_adapter_cleanup_complete(adapter, step) :
            nrf_cs_pair_adapter_transport_error(adapter, step);
    case NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY:
        return nrf_cs_pair_adapter_measurement_ready(adapter, step, measurement);
    case NRF_CS_PAIR_BRIDGE_TIMEOUT:
        return nrf_cs_pair_adapter_timeout(adapter, step);
    case NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE:
        return nrf_cs_pair_adapter_cleanup_complete(adapter, step);
    default:
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
}

static inline bool nrf_cs_pair_runtime_bridge_record_completion(
    bool *local_complete,
    bool *peer_complete,
    bool local_event)
{
    if (local_complete == NULL || peer_complete == NULL) {
        return false;
    }
    if (local_event) {
        *local_complete = true;
    } else {
        *peer_complete = true;
    }
    return *local_complete && *peer_complete;
}

#endif
