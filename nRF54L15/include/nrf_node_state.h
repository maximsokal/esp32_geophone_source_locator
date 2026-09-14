#ifndef NRF_NODE_STATE_H
#define NRF_NODE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_NODE_ROLE_UNASSIGNED = 0,
    NRF_NODE_ROLE_MAIN,
    NRF_NODE_ROLE_REMOTE,
} nrf_node_role_t;

typedef enum {
    NRF_NODE_LIFECYCLE_BOOTING = 0,
    NRF_NODE_LIFECYCLE_READY,
    NRF_NODE_LIFECYCLE_UNAVAILABLE,
} nrf_node_lifecycle_t;

typedef enum {
    NRF_NODE_STATE_OK = 0,
    NRF_NODE_STATE_ERR_ARGUMENT = -1,
    NRF_NODE_STATE_ERR_NOT_READY = -2,
    NRF_NODE_STATE_ERR_ROLE_CONFLICT = -3,
} nrf_node_state_result_t;

typedef struct {
    nrf_node_role_t role;
    nrf_node_lifecycle_t lifecycle;
} nrf_node_state_t;

void nrf_node_state_init(nrf_node_state_t *state);
void nrf_node_state_reset(nrf_node_state_t *state);

nrf_node_state_result_t nrf_node_state_mark_ready(nrf_node_state_t *state);
nrf_node_state_result_t nrf_node_state_assign_role(
    nrf_node_state_t *state,
    nrf_node_role_t role);
nrf_node_state_result_t nrf_node_state_mark_unavailable(nrf_node_state_t *state);
nrf_node_state_result_t nrf_node_state_mark_available(nrf_node_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
