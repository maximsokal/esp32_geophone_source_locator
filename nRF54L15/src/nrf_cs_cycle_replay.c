#include "nrf_cs_cycle_replay.h"

#include <stdbool.h>

int nrf_cs_cycle_replay(nrf_cs_scheduler_t *scheduler, const nrf_cs_cycle_node_records_t *nodes, size_t node_count)
{
    int missing = 0;
    nrf_cs_step_t step;

    if (scheduler == NULL || (node_count > 0U && nodes == NULL)) return NRF_CS_ERR_ARGUMENT;
    for (;;) {
        const nrf_cs_result_t next = nrf_cs_scheduler_next_step(scheduler, &step);
        if (next == NRF_CS_CYCLE_COMPLETE) return missing;
        if (next != NRF_CS_STEP_READY) return (int)next;

        nrf_cs_measurement_t measurement = {.outcome = NRF_CS_OUTCOME_TIMEOUT};
        bool found = false;
        const size_t execution = scheduler->active_execution_index;
        for (size_t n = 0U; n < node_count && !found; ++n) {
            if (nodes[n].node != step.initiator) continue;
            for (size_t i = 0U; i < nodes[n].count; ++i) {
                const nrf_cs_cycle_record_t *record = &nodes[n].records[i];
                if (record->execution == execution && record->attempt == step.attempt_number) {
                    nrf_cs_cycle_record_to_measurement(record, &measurement);
                    found = true;
                    break;
                }
            }
        }
        if (!found) ++missing;
        const nrf_cs_result_t done = nrf_cs_scheduler_complete_step(scheduler, &step, &measurement);
        if (done != NRF_CS_OK) return (int)done;
    }
}
