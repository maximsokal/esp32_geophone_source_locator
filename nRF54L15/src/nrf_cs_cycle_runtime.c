#include "nrf_cs_cycle_runtime.h"

#include <string.h>

#define WINDOW_NONE 0xFFU

static uint64_t now_ms(const nrf_cs_cycle_runtime_t *rt)
{
    return rt->ops.now_ms != NULL ? rt->ops.now_ms(rt->ops.context) : 0U;
}

void nrf_cs_cycle_runtime_init(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_runtime_ops_t *ops)
{
    if (rt == NULL || ops == NULL) return;
    memset(rt, 0, sizeof(*rt));
    rt->ops = *ops;
    rt->state = NRF_CS_CYCLE_STATE_IDLE;
    rt->window = WINDOW_NONE;
}

static void forget_cycle(nrf_cs_cycle_runtime_t *rt)
{
    const nrf_cs_cycle_runtime_ops_t ops = rt->ops;
    const uint32_t next_attempt_id = rt->next_attempt_id;
    nrf_cs_cycle_runtime_init(rt, &ops);
    rt->next_attempt_id = next_attempt_id; /* attempt ids never repeat within a boot */
}

int nrf_cs_cycle_runtime_accept_plan(nrf_cs_cycle_runtime_t *rt, nrf_cs_node_id_t local_node, const nrf_cs_cycle_plan_t *plan)
{
    uint8_t scratch[NRF_CS_CYCLE_PLAN_LENGTH];

    if (rt == NULL || plan == NULL || local_node == 0xFFU) return -1;
    if (rt->state == NRF_CS_CYCLE_STATE_RUNNING) return -2;
    /* The encoder applies the same validation as the wire decoder. */
    if (nrf_cs_cycle_encode_plan(plan, scratch) != 0 || plan->self_node != local_node) return -1;

    forget_cycle(rt);
    rt->plan_valid = true;
    rt->local_node = local_node;
    rt->cycle_id = plan->cycle_id;
    rt->membership_generation = plan->membership_generation;
    rt->pair_timeout_ms = plan->pair_timeout_ms;
    rt->max_attempts = plan->max_attempts;
    rt->samples_per_pair = plan->samples_per_pair;
    rt->min_confidence = plan->min_confidence;
    rt->step_count = plan->step_count;
    return 0;
}

int nrf_cs_cycle_runtime_accept_steps(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_plan_steps_t *chunk)
{
    uint8_t scratch[NRF_CS_CYCLE_PLAN_STEPS_LENGTH];

    if (rt == NULL || chunk == NULL || !rt->plan_valid) return -1;
    if (rt->state == NRF_CS_CYCLE_STATE_RUNNING) return -2;
    if (chunk->cycle_id != rt->cycle_id || nrf_cs_cycle_encode_plan_steps(chunk, scratch) != 0 ||
        (unsigned)chunk->first_step + chunk->step_count > rt->step_count) {
        return -1;
    }
    for (uint8_t i = 0U; i < chunk->step_count; ++i) {
        rt->steps[chunk->first_step + i] = chunk->steps[i];
        rt->steps_filled |= UINT32_C(1) << (chunk->first_step + i);
    }
    return 0;
}

bool nrf_cs_cycle_runtime_plan_ready(const nrf_cs_cycle_runtime_t *rt)
{
    if (rt == NULL || !rt->plan_valid || rt->step_count == 0U) return false;
    const uint32_t all = (UINT32_C(1) << rt->step_count) - 1U;
    return (rt->steps_filled & all) == all;
}

int nrf_cs_cycle_runtime_go(nrf_cs_cycle_runtime_t *rt, const nrf_cs_cycle_go_t *go)
{
    if (rt == NULL || go == NULL || go->request_id == 0U || !nrf_cs_cycle_runtime_plan_ready(rt) ||
        go->cycle_id != rt->cycle_id) {
        return -1;
    }
    if (rt->state != NRF_CS_CYCLE_STATE_IDLE) {
        return go->request_id == rt->go_request_id ? 0 : -2;
    }
    const uint32_t window_ms = nrf_cs_cycle_window_ms(
        (uint16_t)rt->pair_timeout_ms, rt->samples_per_pair, rt->max_attempts);
    const uint32_t run_ms = nrf_cs_cycle_run_ms(rt->step_count, window_ms);
    if (go->total_timeout_ms < run_ms) return -1;

    rt->go_request_id = go->request_id;
    rt->window_ms = window_ms;
    rt->run_ms = run_ms;
    rt->t0_ms = now_ms(rt);
    rt->window = WINDOW_NONE;
    rt->record_count = 0U;
    rt->failure = NRF_CS_CYCLE_FAILURE_NONE;
    rt->released_mask = 0U;
    rt->closed_mask = 0U;
    rt->last_role = WINDOW_NONE;
    rt->relay = rt->step_count >= 2U;
    for (uint8_t e = 0U; e < rt->step_count; ++e) {
        if (rt->steps[e].initiator == rt->local_node || rt->steps[e].reflector == rt->local_node) rt->last_role = e;
        if (e > 0U && rt->steps[e].initiator != rt->steps[e - 1U].reflector) rt->relay = false;
    }
    rt->state = NRF_CS_CYCLE_STATE_RUNNING;
    return 0;
}

static uint32_t bit_of(uint8_t execution)
{
    return UINT32_C(1) << execution;
}

/* The window the node owns is over (its execution finished, ran out of room, or was cut): remember it. */
static void mark_closed(nrf_cs_cycle_runtime_t *rt)
{
    if (rt->window != WINDOW_NONE && rt->steps[rt->window].initiator == rt->local_node) {
        rt->closed_mask |= bit_of(rt->window);
    }
}

static void add_record(nrf_cs_cycle_runtime_t *rt, uint8_t attempt, const nrf_cs_measurement_t *measurement)
{
    if (rt->record_count < NRF_CS_CYCLE_MAX_RECORDS) {
        nrf_cs_cycle_record_from_measurement(rt->window, attempt, measurement, &rt->records[rt->record_count++]);
    }
}

/* Same stop rule as nrf_cs_scheduler_complete_step. */
static void finish_attempt(nrf_cs_cycle_runtime_t *rt, const nrf_cs_measurement_t *measurement)
{
    const bool accepted = measurement->outcome == NRF_CS_OUTCOME_SUCCESS &&
        measurement->confidence >= rt->min_confidence;

    ++rt->attempts;
    if (accepted) ++rt->successes;
    add_record(rt, rt->attempts, measurement);
    rt->attempt_active = false;
    rt->attempt_ready_ms = now_ms(rt) + NRF_CS_CYCLE_ATTEMPT_GAP_MS;
    if (rt->successes >= rt->samples_per_pair ||
        (uint8_t)(rt->attempts - rt->successes) >= rt->max_attempts) {
        rt->execution_done = true;
        mark_closed(rt);
    }
}

bool nrf_cs_cycle_runtime_would_finish(const nrf_cs_cycle_runtime_t *rt, const nrf_cs_measurement_t *measurement)
{
    if (rt == NULL || measurement == NULL || rt->state != NRF_CS_CYCLE_STATE_RUNNING || !rt->attempt_active) return false;
    const bool accepted = measurement->outcome == NRF_CS_OUTCOME_SUCCESS && measurement->confidence >= rt->min_confidence;
    const unsigned attempts = (unsigned)rt->attempts + 1U;
    const unsigned successes = (unsigned)rt->successes + (accepted ? 1U : 0U);
    if (successes >= rt->samples_per_pair || attempts - successes >= rt->max_attempts) return true;
    /* The next attempt cannot start earlier than this, so if it does not fit now it never will. */
    const uint64_t ready = now_ms(rt) + NRF_CS_CYCLE_ATTEMPT_GAP_MS;
    return rt->window_deadline_ms < ready || rt->window_deadline_ms - ready < rt->pair_timeout_ms;
}

static void close_window(nrf_cs_cycle_runtime_t *rt)
{
    mark_closed(rt);
    if (rt->attempt_active) {
        rt->start_pending = false; /* an attempt that was never taken counts as timed out too */
        if (rt->ops.abort_attempt != NULL) rt->ops.abort_attempt(rt->ops.context);
        /* The attempt ran out of window: the ESP32 sees it as a timeout. */
        const nrf_cs_measurement_t timed_out = {.outcome = NRF_CS_OUTCOME_TIMEOUT};
        finish_attempt(rt, &timed_out);
    }
}

static void open_window(nrf_cs_cycle_runtime_t *rt, uint8_t window, uint64_t ready_ms)
{
    rt->window = window;
    rt->window_deadline_ms = rt->t0_ms + ((uint64_t)window + 1U) * rt->window_ms;
    rt->execution_done = false;
    rt->attempt_active = false;
    rt->start_pending = false;
    rt->window_exhausted = false;
    rt->attempts = 0U;
    rt->successes = 0U;
    rt->attempt_ready_ms = ready_ms;
}

/* All of this node's own executions are over and it has no reflector duty left: nothing more to do in this cycle. */
static bool duties_over(const nrf_cs_cycle_runtime_t *rt)
{
    if (!rt->relay || rt->last_role == WINDOW_NONE) return false;
    for (uint8_t e = 0U; e < rt->step_count; ++e) {
        if (rt->steps[e].initiator == rt->local_node && (rt->closed_mask & bit_of(e)) == 0U) return false;
    }
    return rt->steps[rt->last_role].initiator == rt->local_node || (rt->released_mask & bit_of(rt->last_role)) != 0U;
}

void nrf_cs_cycle_runtime_peer_released(nrf_cs_cycle_runtime_t *rt)
{
    if (rt == NULL || rt->state != NRF_CS_CYCLE_STATE_RUNNING || !rt->relay) return;
    const uint64_t now = now_ms(rt);
    uint8_t first = rt->window == WINDOW_NONE ? 0U : rt->window;
    /* No later chain execution can finish before our first attempt in this still-open window. */
    if (first > 0U && first < rt->step_count && rt->steps[first].initiator == rt->local_node &&
        rt->steps[first - 1U].reflector == rt->local_node &&
        (rt->released_mask & bit_of((uint8_t)(first - 1U))) == 0U &&
        rt->attempts == 0U && !rt->attempt_active && now < rt->window_deadline_ms) {
        --first;
    }
    /* The chain is linear: the execution being served is the first reflector duty not yet released, from the window
     * this node has reached on (a duty before it was passed over by time and its release was lost). */
    for (uint8_t e = first; e < rt->step_count; ++e) {
        if (rt->steps[e].reflector != rt->local_node || (rt->released_mask & bit_of(e)) != 0U) continue;
        rt->released_mask |= bit_of(e);
        /* Its own window may already be open and waiting for the slot's start: shorten the wait. */
        const uint64_t ready = now + NRF_CS_CYCLE_RELAY_SETTLE_MS;
        if ((uint8_t)(e + 1U) == rt->window && rt->attempts == 0U && !rt->attempt_active && ready < rt->attempt_ready_ms) {
            rt->attempt_ready_ms = ready;
        }
        return;
    }
}

static void start_attempt(nrf_cs_cycle_runtime_t *rt)
{
    const nrf_cs_cycle_step_t *plan_step = &rt->steps[rt->window];

    memset(&rt->active_step, 0, sizeof(rt->active_step));
    rt->next_attempt_id = rt->next_attempt_id == UINT32_MAX ? 1U : rt->next_attempt_id + 1U;
    rt->active_step.cycle_id = rt->cycle_id;
    rt->active_step.membership_generation = rt->membership_generation;
    rt->active_step.attempt_id = rt->next_attempt_id;
    rt->active_step.pair_index = (uint8_t)(rt->window % NRF_CS_MAX_PAIRS);
    rt->active_step.attempt_number = (uint8_t)(rt->attempts + 1U);
    rt->active_step.initiator = plan_step->initiator;
    rt->active_step.reflector = plan_step->reflector;
    rt->active_step.timeout_ms = rt->pair_timeout_ms;
    rt->attempt_active = true;
    rt->start_pending = true;
}

bool nrf_cs_cycle_runtime_take_start(nrf_cs_cycle_runtime_t *rt, nrf_cs_step_t *step)
{
    if (rt == NULL || step == NULL || !rt->start_pending) return false;
    rt->start_pending = false;
    *step = rt->active_step;
    return true;
}

void nrf_cs_cycle_runtime_start_result(nrf_cs_cycle_runtime_t *rt, const nrf_cs_step_t *step, int result)
{
    if (rt == NULL || step == NULL || result == 0 || !rt->attempt_active ||
        step->attempt_id != rt->active_step.attempt_id) {
        return; /* started, or the outcome already arrived and the attempt is over */
    }
    if (result > 0) {
        rt->attempt_active = false;
        rt->attempt_ready_ms = now_ms(rt) + NRF_CS_CYCLE_BUSY_RETRY_MS;
        return;
    }
    /* The BLE side refused to start: a failed attempt, not a stall. */
    const nrf_cs_measurement_t refused = {.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR};
    finish_attempt(rt, &refused);
}

void nrf_cs_cycle_runtime_tick(nrf_cs_cycle_runtime_t *rt)
{
    if (rt == NULL) return;

    const uint64_t now = now_ms(rt);
    if (rt->state == NRF_CS_CYCLE_STATE_COMPLETE || rt->state == NRF_CS_CYCLE_STATE_FAILED) {
        /* An early local finish must not discard records before the coordinator collects the global cycle. */
        if (now >= rt->finished_ms && now - rt->finished_ms >= NRF_CS_CYCLE_REPLY_HOLD_MS &&
            now >= rt->t0_ms && now - rt->t0_ms >= (uint64_t)rt->run_ms + NRF_CS_CYCLE_REPLY_HOLD_MS) {
            forget_cycle(rt);
        }
        return;
    }
    if (rt->state != NRF_CS_CYCLE_STATE_RUNNING) return;

    const uint64_t elapsed = now >= rt->t0_ms ? now - rt->t0_ms : 0U;

    /* Relay: everything this node had to do is done before the last slot ends. */
    if (!rt->attempt_active && duties_over(rt)) {
        if (rt->window != WINDOW_NONE) close_window(rt);
        rt->finished_ms = now;
        rt->state = NRF_CS_CYCLE_STATE_COMPLETE;
        return;
    }
    if (elapsed >= rt->run_ms) {
        if (rt->window != WINDOW_NONE) close_window(rt);
        rt->finished_ms = now;
        rt->state = NRF_CS_CYCLE_STATE_COMPLETE;
        return;
    }
    uint8_t window = (uint8_t)(elapsed / rt->window_ms);
    bool early = false;
    if (rt->relay) {
        /* The initiator before us let go: our execution may start now, ahead of its slot. */
        for (uint8_t e = (uint8_t)(window + 1U); e < rt->step_count; ++e) {
            if (rt->steps[e].initiator == rt->local_node && (rt->closed_mask & bit_of(e)) == 0U &&
                (rt->released_mask & bit_of((uint8_t)(e - 1U))) != 0U) {
                window = e;
                early = true;
            }
        }
    }
    if (rt->window != WINDOW_NONE && window < rt->window) window = rt->window; /* never go back */
    if (window != rt->window) {
        const uint64_t slot_ready = rt->t0_ms + (uint64_t)window * rt->window_ms + NRF_CS_CYCLE_GO_SKEW_MS;
        const uint64_t relay_ready = now + NRF_CS_CYCLE_RELAY_SETTLE_MS;
        const bool released = window > 0U && rt->steps[window].initiator == rt->local_node &&
            (rt->released_mask & bit_of((uint8_t)(window - 1U))) != 0U;
        if (rt->window != WINDOW_NONE) close_window(rt);
        open_window(rt, window, (early || released) && relay_ready < slot_ready ? relay_ready : slot_ready);
    }
    if (rt->steps[window].initiator != rt->local_node || rt->execution_done || rt->attempt_active ||
        rt->window_exhausted) {
        return;
    }
    if (now < rt->attempt_ready_ms) return;
    if (rt->window_deadline_ms < now || rt->window_deadline_ms - now < rt->pair_timeout_ms) {
        rt->window_exhausted = true; /* no room for another full attempt */
        mark_closed(rt);
        return;
    }
    start_attempt(rt);
}

int nrf_cs_cycle_runtime_step_done(nrf_cs_cycle_runtime_t *rt, const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement)
{
    if (rt == NULL || step == NULL || measurement == NULL || rt->state != NRF_CS_CYCLE_STATE_RUNNING ||
        !rt->attempt_active || step->attempt_id != rt->active_step.attempt_id ||
        step->cycle_id != rt->active_step.cycle_id) {
        return -1;
    }
    if (measurement->outcome > NRF_CS_OUTCOME_OUTLIER || measurement->confidence > NRF_CS_CONFIDENCE_MAX ||
        (measurement->outcome == NRF_CS_OUTCOME_SUCCESS && measurement->distance_mm < 0)) {
        return -1;
    }
    /* BLE callbacks may precede the main-loop tick that closes this window. */
    const uint64_t window_end = rt->t0_ms + ((uint64_t)rt->window + 1U) * rt->window_ms;
    if (now_ms(rt) >= window_end) {
        const nrf_cs_measurement_t timed_out = {.outcome = NRF_CS_OUTCOME_TIMEOUT};
        finish_attempt(rt, &timed_out);
        return 0;
    }
    finish_attempt(rt, measurement);
    return 0;
}

void nrf_cs_cycle_runtime_abort(nrf_cs_cycle_runtime_t *rt, uint8_t failure)
{
    if (rt == NULL || rt->state != NRF_CS_CYCLE_STATE_RUNNING) return;
    if (rt->attempt_active && rt->ops.abort_attempt != NULL) rt->ops.abort_attempt(rt->ops.context);
    rt->attempt_active = false;
    rt->start_pending = false;
    rt->failure = failure;
    rt->finished_ms = now_ms(rt);
    rt->state = NRF_CS_CYCLE_STATE_FAILED;
}

void nrf_cs_cycle_runtime_release(nrf_cs_cycle_runtime_t *rt)
{
    if (rt == NULL) return;
    if (rt->state == NRF_CS_CYCLE_STATE_RUNNING) nrf_cs_cycle_runtime_abort(rt, NRF_CS_CYCLE_FAILURE_ABORTED);
    forget_cycle(rt);
}

bool nrf_cs_cycle_runtime_running(const nrf_cs_cycle_runtime_t *rt)
{
    return rt != NULL && rt->state == NRF_CS_CYCLE_STATE_RUNNING;
}

bool nrf_cs_cycle_runtime_reply_active(const nrf_cs_cycle_runtime_t *rt)
{
    return rt != NULL && rt->state != NRF_CS_CYCLE_STATE_IDLE;
}

void nrf_cs_cycle_runtime_status(const nrf_cs_cycle_runtime_t *rt, uint32_t request_id, nrf_cs_cycle_status_t *status)
{
    if (rt == NULL || status == NULL) return;
    memset(status, 0, sizeof(*status));
    status->request_id = request_id != 0U ? request_id : (rt->go_request_id != 0U ? rt->go_request_id : 1U);
    status->cycle_id = rt->state == NRF_CS_CYCLE_STATE_IDLE ? 0U : rt->cycle_id;
    status->state = (uint8_t)rt->state;
    status->failure = rt->failure;
    status->pairs_total = rt->step_count;
    if (rt->state == NRF_CS_CYCLE_STATE_RUNNING) {
        const uint64_t now = now_ms(rt);
        const uint64_t elapsed = now >= rt->t0_ms ? now - rt->t0_ms : 0U;
        status->elapsed_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
        status->pairs_done = rt->window == WINDOW_NONE ? 0U : rt->window;
    } else if (rt->state == NRF_CS_CYCLE_STATE_COMPLETE) {
        status->elapsed_ms = rt->run_ms;
        status->pairs_done = rt->step_count;
    }
    if (status->pairs_done > status->pairs_total) status->pairs_done = status->pairs_total;
}

int nrf_cs_cycle_runtime_result_page(const nrf_cs_cycle_runtime_t *rt, uint32_t request_id, uint8_t page, nrf_cs_cycle_result_t *result)
{
    if (rt == NULL || result == NULL || request_id == 0U || rt->state == NRF_CS_CYCLE_STATE_IDLE ||
        rt->record_count == 0U) {
        return -1;
    }
    const size_t page_count = nrf_cs_cycle_result_page_count(rt->record_count);
    if (page >= page_count) return -1;

    memset(result, 0, sizeof(*result));
    result->request_id = request_id;
    result->cycle_id = rt->cycle_id;
    result->page = page;
    result->page_count = (uint8_t)page_count;
    result->record_total = (uint8_t)rt->record_count;
    const size_t before = (size_t)page * NRF_CS_CYCLE_RESULTS_PER_PAGE;
    const size_t left = rt->record_count - before;
    result->record_count = (uint8_t)(left < NRF_CS_CYCLE_RESULTS_PER_PAGE ? left : NRF_CS_CYCLE_RESULTS_PER_PAGE);
    memcpy(result->records, &rt->records[before], (size_t)result->record_count * sizeof(result->records[0]));
    return 0;
}
