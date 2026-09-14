#ifndef NRF_BOOTSTRAP_RUNTIME_ZEPHYR_H
#define NRF_BOOTSTRAP_RUNTIME_ZEPHYR_H

#include "nrf_node_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int nrf_bootstrap_runtime_zephyr_start(nrf_node_state_t *node_state);

#ifdef __cplusplus
}
#endif

#endif
