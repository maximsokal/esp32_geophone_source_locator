#ifndef NRF_CS_PAIR_RUNTIME_ZEPHYR_H
#define NRF_CS_PAIR_RUNTIME_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>
#include "nrf_cs_scheduler.h"
void nrf_cs_pair_runtime_zephyr_cancel(const nrf_cs_step_t *step);

typedef void (*nrf_cs_pair_runtime_complete_cb_t)(
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement,
    void *context);

int nrf_cs_pair_runtime_zephyr_start(void);
int nrf_cs_pair_runtime_zephyr_set_node_id(nrf_cs_node_id_t node_id);
int nrf_cs_pair_runtime_zephyr_set_radio_enabled(bool enabled);
typedef bool (*nrf_cs_pair_runtime_final_hint_cb_t)(const nrf_cs_measurement_t *measurement);
typedef void (*nrf_cs_pair_runtime_released_cb_t)(void);
void nrf_cs_pair_runtime_zephyr_set_relay_hooks(
    nrf_cs_pair_runtime_final_hint_cb_t final_hint, nrf_cs_pair_runtime_released_cb_t released);
int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context);
bool nrf_cs_pair_runtime_zephyr_is_busy(void);
bool nrf_cs_pair_runtime_zephyr_incoming_idle_ms(uint32_t *idle_ms);

#endif
