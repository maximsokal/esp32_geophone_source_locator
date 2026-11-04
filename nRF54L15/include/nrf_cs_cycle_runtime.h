#ifndef NRF_CS_CYCLE_RUNTIME_H
#define NRF_CS_CYCLE_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_cs_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Node side of an autonomous Channel Sounding cycle (platform neutral).
 *
 * The ESP32 gives every node the same ordered list of directed executions (the plan) and
 * starts all nodes with GO. From then on time alone coordinates them: execution e owns the
 * window [t0 + e * window, t0 + (e + 1) * window). The node that is the initiator of
 * execution e runs attempts inside its window (the other nodes just advertise as reflectors),
 * applying the same stop rule as nrf_cs_scheduler: stop after samples_per_pair accepted
 * samples or max_attempts failures. Every attempt is stored as a record; the ESP32 reads the
 * records back after the cycle and replays them through the scheduler.
 *
 * Not thread safe: call everything from one context (the CS work queue).
 */

/* Pause after an attempt for BLE cleanup and re-advertising. */
#define NRF_CS_CYCLE_ATTEMPT_GAP_MS 150U
/* GO reaches the nodes one after another; the first attempt of a window waits this long. */
#define NRF_CS_CYCLE_GO_SKEW_MS 300U
/*
 * Relay (plan order from nrf_cs_policy_t.relay_order): the reflector of execution e is the initiator of e + 1. The
 * initiator that finishes its execution disconnects with this reason instead of the ordinary one; the node on the other
 * end then knows its turn has come and starts after RELAY_SETTLE_MS instead of waiting for its time slot. The reason is
 * HCI "remote device terminated the connection due to power off", one of the few a Disconnect command may carry; here it
 * only means "that was my last attempt of this execution". A release that never comes costs nothing but time: the node
 * starts at its slot as before.
 */
#define NRF_CS_CYCLE_RELEASE_REASON 0x15U
#define NRF_CS_CYCLE_RELAY_SETTLE_MS 150U
/* Time after the last window before the ESP32 may stop waiting. */
#define NRF_CS_CYCLE_TOTAL_MARGIN_MS 3000U
/* The BLE side was busy when an attempt was due; try again after this long (not a failed attempt). */
#define NRF_CS_CYCLE_BUSY_RETRY_MS 50U
/*
 * A finished cycle keeps answering with CS_CYCLE_STATUS until the ESP32 releases it. If the ESP32
 * never does (reset mid-collection), the node forgets the cycle this long after the later of its
 * local finish and the planned end of all execution windows, so normal STATUS reads work again.
 */
#define NRF_CS_CYCLE_REPLY_HOLD_MS 120000U

/* Length of one execution window for a policy; ESP32 and nodes must agree on it. */
static inline uint32_t nrf_cs_cycle_window_ms(uint16_t pair_timeout_ms, uint8_t samples_per_pair, uint8_t max_attempts)
{
    const uint32_t attempts = (uint32_t)samples_per_pair + max_attempts - 1U;
    return NRF_CS_CYCLE_GO_SKEW_MS + attempts * ((uint32_t)pair_timeout_ms + NRF_CS_CYCLE_ATTEMPT_GAP_MS);
}

/* Time from GO until every window has closed. */
static inline uint32_t nrf_cs_cycle_run_ms(uint8_t step_count, uint32_t window_ms)
{
    return (uint32_t)step_count * window_ms;
}

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
    /* Cancel an attempt that is still running when its window closes. */
    void (*abort_attempt)(void *context);
} nrf_cs_cycle_runtime_ops_t;

typedef struct {
    nrf_cs_cycle_runtime_ops_t ops;
    nrf_cs_cycle_state_t state;
    uint8_t failure;
    bool plan_valid;
    nrf_cs_node_id_t local_node;
    uint32_t cycle_id;
    uint32_t membership_generation;
    uint32_t pair_timeout_ms;
    uint8_t max_attempts;
    uint8_t samples_per_pair;
    uint16_t min_confidence;
    uint8_t step_count;
    uint32_t steps_filled;
    nrf_cs_cycle_step_t steps[NRF_CS_CYCLE_MAX_STEPS];

    uint32_t go_request_id;
    uint64_t t0_ms;
    uint64_t finished_ms;
    uint32_t window_ms;
    uint32_t run_ms;
    uint8_t window; /* execution whose window is open; 0xFF before the first tick */
    bool execution_done;
    bool attempt_active;
    bool window_exhausted;
    bool start_pending;
    uint8_t attempts;
    uint8_t successes;
    uint64_t attempt_ready_ms;
    uint32_t next_attempt_id;
    nrf_cs_step_t active_step;

    /* Relay (see NRF_CS_CYCLE_RELEASE_REASON); all zero when the plan is not a chain. */
    bool relay;                 /* every execution starts at the node the previous one measured */
    uint8_t last_role;          /* last execution this node takes part in, either way; 0xFF if none */
    uint32_t released_mask;     /* bit e: as reflector of e this node saw the initiator let go */
    uint32_t closed_mask;       /* bit e: this node's own execution e is over */
    uint64_t window_deadline_ms;

    uint16_t record_count;
    nrf_cs_cycle_record_t records[NRF_CS_CYCLE_MAX_RECORDS];
} nrf_cs_cycle_runtime_t;

void nrf_cs_cycle_runtime_init(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_runtime_ops_t *ops);

/*
 * Plan intake. All return 0 on success, -1 for invalid input, -2 when a cycle is running.
 * The plan frame carries the receiver's own node id; it must equal local_node.
 */
int nrf_cs_cycle_runtime_accept_plan(nrf_cs_cycle_runtime_t *rt, nrf_cs_node_id_t local_node, const nrf_cs_cycle_plan_t *plan);
int nrf_cs_cycle_runtime_accept_steps(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_plan_steps_t *chunk);
bool nrf_cs_cycle_runtime_plan_ready(const nrf_cs_cycle_runtime_t *rt);

/* Start the cycle now. Repeating the same GO request is harmless. */
int nrf_cs_cycle_runtime_go(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_go_t *go);

/* Drive the cycle; call every ~50 ms while a cycle runs. */
void nrf_cs_cycle_runtime_tick(nrf_cs_cycle_runtime_t *rt);

/*
 * The tick only prepares an attempt. The caller takes it and starts the BLE side outside any
 * lock, then reports how that went:
 *   result 0   started; the outcome arrives through nrf_cs_cycle_runtime_step_done()
 *   result > 0 BLE busy right now; the attempt is retried shortly and costs nothing
 *   result < 0 refused; recorded as a failed attempt so the cycle cannot stall
 */
bool nrf_cs_cycle_runtime_take_start(nrf_cs_cycle_runtime_t *rt, nrf_cs_step_t *step);
void nrf_cs_cycle_runtime_start_result(nrf_cs_cycle_runtime_t *rt, const nrf_cs_step_t *step, int result);

/* Outcome of the attempt started through ops->execute. Stale tokens are ignored (-1). */
int nrf_cs_cycle_runtime_step_done(nrf_cs_cycle_runtime_t *rt, const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement);

/*
 * The initiator that was connected to this node disconnected with NRF_CS_CYCLE_RELEASE_REASON: the execution this node
 * was the reflector of is over. Safe to call at any time; ignored when no cycle runs or it is not a relay plan.
 */
void nrf_cs_cycle_runtime_peer_released(nrf_cs_cycle_runtime_t *rt);

/*
 * Whether the attempt that is running now ends the execution if it finishes with `measurement` (the same stop rule as
 * finish_attempt, plus "no room left in the window for another attempt"). The caller uses it to pick the disconnect
 * reason; it never says true when more attempts will follow.
 */
bool nrf_cs_cycle_runtime_would_finish(const nrf_cs_cycle_runtime_t *rt, const nrf_cs_measurement_t *measurement);

/* Stop a running cycle (state becomes FAILED with the given reason). */
void nrf_cs_cycle_runtime_abort(nrf_cs_cycle_runtime_t *rt, uint8_t failure);

/* Forget the cycle after the ESP32 collected it; STATUS replies stop. */
void nrf_cs_cycle_runtime_release(nrf_cs_cycle_runtime_t *rt);

bool nrf_cs_cycle_runtime_running(const nrf_cs_cycle_runtime_t *rt);
/* True while STATUS reads must answer with CS_CYCLE_STATUS instead of the legacy STATUS. */
bool nrf_cs_cycle_runtime_reply_active(const nrf_cs_cycle_runtime_t *rt);

void nrf_cs_cycle_runtime_status(const nrf_cs_cycle_runtime_t *rt, uint32_t request_id, nrf_cs_cycle_status_t *status);
/* Fills one result page; returns 0, or -1 when there is nothing for that page. */
int nrf_cs_cycle_runtime_result_page(const nrf_cs_cycle_runtime_t *rt, uint32_t request_id, uint8_t page, nrf_cs_cycle_result_t *result);

#ifdef __cplusplus
}
#endif
#endif
