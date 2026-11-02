#ifndef NRF_CS_CYCLE_COORDINATOR_H
#define NRF_CS_CYCLE_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_cs_cycle_replay.h"
#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ESP32 side of an autonomous Channel Sounding cycle (platform neutral).
 *
 *   1. hand every node the plan (policy + ordered directed executions),
 *   2. start all nodes with GO,
 *   3. stay off the bus while the nodes range over Bluetooth; only MAIN's status is read
 *      now and then,
 *   4. when the cycle is over read every initiating node's attempt records and release it.
 *
 * The caller replays the collected records through the scheduler (nrf_cs_cycle_replay) so
 * pair results come out exactly as if the ESP32 had driven every step. The caller also holds
 * the quiet bus lease around nrf_cs_cycle_coordinate(); the coordinator itself only does
 * I2C through the ops below.
 */

/* Time between MAIN status reads while the nodes are ranging. */
#define NRF_CS_COORD_POLL_PERIOD_MS 5000U
/* Time between MAIN status reads once the cycle should be over. */
#define NRF_CS_COORD_POLL_FAST_MS 500U
#define NRF_CS_COORD_WRITE_TRIES 3U
#define NRF_CS_COORD_GO_TRIES 2U
#define NRF_CS_COORD_PAGE_TRIES 6U
/* Tries per node for a release that must not hold up an abort (shutdown, BACK). */
#define NRF_CS_COORD_RELEASE_TRIES 2U
#define NRF_CS_COORD_RETRY_PAUSE_MS 300U
/* Extra time for collecting results after the cycle deadline. */
#define NRF_CS_COORD_COLLECT_GRACE_MS 6000U
/* Time per node for plan, GO, result reads and release on a healthy bus, with room for retries. */
#define NRF_CS_COORD_NODE_OVERHEAD_MS 2500U
/* Pause in the wait loop so an abort request is noticed quickly. */
#define NRF_CS_COORD_SLEEP_SLICE_MS 100U

typedef enum {
    NRF_CS_COORD_OK = 0,
    /* The cycle ran but some initiating node's records could not be read back. */
    NRF_CS_COORD_PARTIAL = 1,
    NRF_CS_COORD_ERR_ARGUMENT = -1,
    /* A node did not take the plan; nothing was started. */
    NRF_CS_COORD_ERR_PLAN = -2,
    /* No node could be started. */
    NRF_CS_COORD_ERR_GO = -3,
    NRF_CS_COORD_ERR_ABORTED = -4,
} nrf_cs_coord_result_t;

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t ms);
    /* Writes one frame to the node's I2C address; 0 on success. */
    int (*write_node)(void *context, nrf_cs_node_id_t node, const uint8_t *frame, size_t length);
    /* Reads `length` bytes from the node (whatever its STATUS reply currently is); 0 on success. */
    int (*read_node)(void *context, nrf_cs_node_id_t node, uint8_t *frame, size_t length);
    /* Optional: true ends the wait early. */
    bool (*should_abort)(void *context);
    /* Optional: MAIN reported progress (windows finished of total). */
    void (*progress)(void *context, uint8_t done, uint8_t total);
} nrf_cs_cycle_coordinator_ops_t;

typedef struct {
    nrf_cs_cycle_record_t records[NRF_CS_CYCLE_MAX_RECORDS];
    size_t record_count;
    nrf_cs_cycle_node_records_t nodes[NRF_CS_MAX_NODES];
    size_t node_count;
} nrf_cs_cycle_collected_t;

typedef struct {
    uint32_t cycle_id;
    uint32_t window_ms;
    uint32_t run_ms;
    /* Bit i = scheduler->nodes[i]. */
    uint8_t plan_mask;
    uint8_t go_mask;
    uint8_t collected_mask;
    uint8_t release_mask;
    /* Nodes whose stale cycle was released before the plan went out: sent = a release frame was
     * acknowledged on the bus, mask = the node was then read back and no longer serves a cycle. */
    uint8_t prerelease_sent_mask;
    uint8_t prerelease_mask;
    /* Nodes released after an abort so none keeps ranging for the rest of the cycle (same meaning). */
    uint8_t abort_release_sent_mask;
    uint8_t abort_release_mask;
    /* MAIN's last readable cycle status. */
    bool main_status_seen;
    bool main_complete;
    uint8_t main_state;
    uint8_t main_failure;
    uint32_t polls;
    uint32_t waited_ms;
} nrf_cs_cycle_report_t;

/* Longest time a cycle for this policy can take, plan to release: use it as the bus lease length. */
uint32_t nrf_cs_cycle_coordinator_budget_ms(const nrf_cs_policy_t *policy, size_t execution_count, size_t node_count);

/*
 * `scheduler` has a started cycle whose executions were not stepped; status_node is the node
 * whose status is polled (MAIN). request_seed numbers the frames. Returns an
 * nrf_cs_coord_result_t; out and report are filled as far as the run got.
 *
 * Every node is released before the plan goes out, so a cycle abandoned earlier (BACK, a lost
 * release, an ESP32 reset) cannot make a node refuse the new plan. When the run is aborted the
 * nodes are released too: a node left alone keeps ranging until its own cycle timer ends, up to
 * run_ms. Those releases ignore should_abort and are best effort. A release only counts once the
 * node has been read back and no longer serves a cycle frame; an acknowledged write is not enough.
 */
int nrf_cs_cycle_coordinate(
    const nrf_cs_cycle_coordinator_ops_t *ops,
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t status_node,
    uint32_t request_seed,
    nrf_cs_cycle_collected_t *out,
    nrf_cs_cycle_report_t *report);

#ifdef __cplusplus
}
#endif
#endif
