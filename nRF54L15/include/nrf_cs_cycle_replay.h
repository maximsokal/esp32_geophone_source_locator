#ifndef NRF_CS_CYCLE_REPLAY_H
#define NRF_CS_CYCLE_REPLAY_H

#include <stddef.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nrf_cs_node_id_t node;
    const nrf_cs_cycle_record_t *records;
    size_t count;
} nrf_cs_cycle_node_records_t;

/*
 * Drives a scheduler whose cycle was already started: every step it issues is answered with
 * the matching recorded attempt of the initiating node (execution index + attempt number). A
 * missing record counts as a TIMEOUT, so a node that never reported cannot stall the replay.
 * Returns the number of missing attempts, or a negative nrf_cs_result_t on a scheduler error.
 */
int nrf_cs_cycle_replay(nrf_cs_scheduler_t *scheduler, const nrf_cs_cycle_node_records_t *nodes, size_t node_count);

#ifdef __cplusplus
}
#endif
#endif
