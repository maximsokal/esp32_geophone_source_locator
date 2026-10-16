#ifndef NRF_CS_CONTROL_RUNTIME_H
#define NRF_CS_CONTROL_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_cs_control_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nrf_cs_node_id_t local_node_id;
    bool assigned;
    bool active;
    nrf_cs_control_request_t active_request;
    bool terminal_valid;
    nrf_cs_control_result_t terminal_result;
    bool immediate_valid;
    nrf_cs_control_result_t immediate_result;
    bool enqueue_result_valid;
    nrf_cs_control_result_t enqueue_result;
    bool selector_armed;
    bool selector_stale;
    nrf_cs_control_request_t selector;
} nrf_cs_control_runtime_t;

int nrf_cs_control_runtime_init(nrf_cs_control_runtime_t *runtime);
int nrf_cs_control_runtime_assign_node(nrf_cs_control_runtime_t *runtime, nrf_cs_node_id_t node_id);
int nrf_cs_control_runtime_accept_execute(nrf_cs_control_runtime_t *runtime, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *status_out, bool *start_required);
int nrf_cs_control_runtime_record_enqueue_result(nrf_cs_control_runtime_t *runtime, const nrf_cs_control_request_t *request, uint8_t command_result);
int nrf_cs_control_runtime_complete(nrf_cs_control_runtime_t *runtime, const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement);
int nrf_cs_control_runtime_select_result(nrf_cs_control_runtime_t *runtime, const nrf_cs_control_request_t *request);
bool nrf_cs_control_runtime_take_selected(nrf_cs_control_runtime_t *runtime, nrf_cs_control_result_t *result_out);

#ifdef __cplusplus
}
#endif
#endif
