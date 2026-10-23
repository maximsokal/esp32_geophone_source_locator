#include <assert.h>
#include <stddef.h>

#include "app_pod_status_state_policy.h"

typedef struct {
    nrf_topology_node_state_t topology_state;
    pod_state_t pod_state;
} state_mapping_t;

int main(void)
{
    static const state_mapping_t mappings[] = {
        {NRF_TOPOLOGY_NODE_UNKNOWN, POD_STATE_DISCONNECTED},
        {NRF_TOPOLOGY_NODE_ABSENT, POD_STATE_DISCONNECTED},
        {NRF_TOPOLOGY_NODE_ASSIGNED, POD_STATE_READY},
        {NRF_TOPOLOGY_NODE_CONFLICT, POD_STATE_ERROR},
        {NRF_TOPOLOGY_NODE_PROTOCOL_ERROR, POD_STATE_ERROR},
        {NRF_TOPOLOGY_NODE_TRANSPORT_ERROR, POD_STATE_DISCONNECTED},
        {(nrf_topology_node_state_t)99, POD_STATE_DISCONNECTED},
    };

    for (size_t index = 0U; index < sizeof(mappings) / sizeof(mappings[0]); ++index) {
        assert(app_pod_status_bridge_map_topology_state(mappings[index].topology_state) ==
            mappings[index].pod_state);
    }

    return 0;
}
