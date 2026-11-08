#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"
#include "nrf_topology.h"

/*
 * Topology mode of one reconcile snapshot.
 *
 * MAIN_PRESENT: the nRF MAIN in the ESP32 housing answered and is ASSIGNED (the existing production mode).
 * REMOTE_ONLY:  MAIN is confirmed ABSENT (0x30, 0x31 and 0x32 all NACKed on the direct bus, never a timeout or a busy bus),
 *               the TCA9548A is usable and the snapshot is fresh. The REMOTEs behind the mux are the whole mesh. MAIN is not
 *               replaced by a REMOTE: node ids, roles and addresses stay what they are.
 * UNCERTAIN:    anything else (UNKNOWN, TRANSPORT_ERROR, PROTOCOL_ERROR, CONFLICT, stale, bus busy, TCA fault or missing,
 *               MAIN absent with no REMOTE at all). Never read as "MAIN absent".
 *
 * remote_count counts only ASSIGNED REMOTEs and is 0 for UNCERTAIN. `complete` is true for the full production set of four
 * REMOTEs; in REMOTE_ONLY fewer is an explicitly partial, diagnostic topology. MAIN_PRESENT never gates on it.
 * The MAIN_PRESENT verdict depends on MAIN alone; staleness and bus state stay with the callers that already check them.
 */
typedef enum {
    NRF_TOPOLOGY_MODE_UNCERTAIN = 0,
    NRF_TOPOLOGY_MODE_MAIN_PRESENT,
    NRF_TOPOLOGY_MODE_REMOTE_ONLY,
} nrf_topology_mode_t;

#define NRF_TOPOLOGY_FULL_REMOTE_COUNT 4U

typedef struct {
    nrf_topology_mode_t mode;
    uint8_t remote_count;
    bool complete;
} nrf_topology_mode_info_t;

nrf_topology_mode_info_t nrf_topology_classify_mode(const nrf_topology_snapshot_t *snapshot);
const char *nrf_topology_mode_name(nrf_topology_mode_t mode);

/*
 * Which node of a Channel Sounding cycle the coordinator asks for status while the nodes range over Bluetooth.
 * MAIN_PRESENT keeps the physical MAIN (only if it is assigned and part of the cycle). Otherwise the lowest node id of the
 * cycle is used: a real, assigned REMOTE (G1 when it is present), never a stand-in MAIN and never node 0 by default.
 * It has no extra Bluetooth role; the initiators keep rotating as the scheduler plans. Returns false for an empty cycle.
 */
bool nrf_topology_select_status_node(
    bool main_assigned,
    nrf_cs_node_id_t main_node_id,
    const nrf_cs_node_id_t *nodes,
    size_t node_count,
    nrf_cs_node_id_t *status_node);
