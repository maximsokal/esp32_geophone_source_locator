#include "nrf_cs_cycle_coordinator.h"

#include <string.h>

#include "nrf_cs_cycle_runtime.h"

typedef struct {
    const nrf_cs_cycle_coordinator_ops_t *ops;
    const nrf_cs_scheduler_t *scheduler;
    nrf_cs_cycle_report_t *report;
    uint32_t request_id;
} coord_t;

static uint8_t normalized_samples(const nrf_cs_policy_t *policy)
{
    return policy->samples_per_pair == 0U ? 1U : policy->samples_per_pair;
}

uint32_t nrf_cs_cycle_coordinator_budget_ms(const nrf_cs_policy_t *policy, size_t execution_count, size_t node_count)
{
    if (policy == NULL || policy->max_attempts == 0U || policy->pair_timeout_ms > UINT16_MAX) return 0U;
    const uint32_t window = nrf_cs_cycle_window_ms(
        (uint16_t)policy->pair_timeout_ms, normalized_samples(policy), policy->max_attempts);
    return nrf_cs_cycle_run_ms((uint8_t)execution_count, window) + NRF_CS_CYCLE_TOTAL_MARGIN_MS +
        NRF_CS_COORD_COLLECT_GRACE_MS + (uint32_t)node_count * NRF_CS_COORD_NODE_OVERHEAD_MS;
}

static uint32_t next_request_id(coord_t *coord)
{
    ++coord->request_id;
    if (coord->request_id == 0U) coord->request_id = 1U;
    return coord->request_id;
}

static bool should_abort(const coord_t *coord)
{
    return coord->ops->should_abort != NULL && coord->ops->should_abort(coord->ops->context);
}

static void pause_ms(const coord_t *coord, uint32_t ms)
{
    coord->ops->sleep_ms(coord->ops->context, ms);
}

static uint64_t now_ms(const coord_t *coord)
{
    return coord->ops->now_ms(coord->ops->context);
}

static int write_with_retries(coord_t *coord, nrf_cs_node_id_t node, const uint8_t *frame, size_t length, unsigned tries)
{
    for (unsigned i = 0U; i < tries; ++i) {
        if (should_abort(coord)) return NRF_CS_COORD_ERR_ABORTED;
        if (coord->ops->write_node(coord->ops->context, node, frame, length) == 0) return 0;
        if (i + 1U < tries) pause_ms(coord, 20U);
    }
    return -1;
}

static size_t node_index(const nrf_cs_scheduler_t *scheduler, nrf_cs_node_id_t node)
{
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        if (scheduler->nodes[i] == node) return i;
    }
    return SIZE_MAX;
}

static bool node_initiates(const nrf_cs_scheduler_t *scheduler, nrf_cs_node_id_t node)
{
    for (size_t e = 0U; e < scheduler->execution_count; ++e) {
        if (scheduler->executions[e].initiator == node) return true;
    }
    return false;
}

/* ---- 1. plan ---- */

static int send_plan(coord_t *coord, nrf_cs_node_id_t node)
{
    const nrf_cs_scheduler_t *scheduler = coord->scheduler;
    const nrf_cs_policy_t *policy = &scheduler->policy;
    uint8_t frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH];
    nrf_cs_cycle_plan_t plan = {
        .request_id = next_request_id(coord),
        .cycle_id = scheduler->cycle_id,
        .membership_generation = (uint16_t)scheduler->cycle_membership_generation,
        .pair_timeout_ms = (uint16_t)policy->pair_timeout_ms,
        .max_attempts = policy->max_attempts,
        .samples_per_pair = normalized_samples(policy),
        .min_confidence = policy->min_confidence,
        .full_rotation = policy->full_rotation,
        .step_count = (uint8_t)scheduler->execution_count,
        .node_count = (uint8_t)scheduler->node_count,
        .self_node = node,
    };
    int rc;

    if (nrf_cs_cycle_encode_plan(&plan, frame) != 0) return -1;
    rc = write_with_retries(coord, node, frame, NRF_CS_CYCLE_PLAN_LENGTH, NRF_CS_COORD_WRITE_TRIES);
    if (rc != 0) return rc;

    for (size_t first = 0U; first < scheduler->execution_count; first += NRF_CS_CYCLE_STEPS_PER_CHUNK) {
        nrf_cs_cycle_plan_steps_t chunk;
        size_t count = scheduler->execution_count - first;
        if (count > NRF_CS_CYCLE_STEPS_PER_CHUNK) count = NRF_CS_CYCLE_STEPS_PER_CHUNK;
        memset(&chunk, 0, sizeof(chunk));
        chunk.request_id = next_request_id(coord);
        chunk.cycle_id = scheduler->cycle_id;
        chunk.first_step = (uint8_t)first;
        chunk.step_count = (uint8_t)count;
        for (size_t i = 0U; i < count; ++i) {
            chunk.steps[i].initiator = scheduler->executions[first + i].initiator;
            chunk.steps[i].reflector = scheduler->executions[first + i].reflector;
        }
        if (nrf_cs_cycle_encode_plan_steps(&chunk, frame) != 0) return -1;
        rc = write_with_retries(coord, node, frame, NRF_CS_CYCLE_PLAN_STEPS_LENGTH, NRF_CS_COORD_WRITE_TRIES);
        if (rc != 0) return rc;
    }
    return 0;
}

/* ---- 2. GO ---- */

static int send_go(coord_t *coord, nrf_cs_node_id_t node, uint32_t total_timeout_ms)
{
    uint8_t frame[NRF_CS_CYCLE_GO_LENGTH];
    const nrf_cs_cycle_go_t go = {
        .request_id = coord->request_id, /* every node gets the same GO */
        .cycle_id = coord->scheduler->cycle_id,
        .total_timeout_ms = total_timeout_ms,
    };
    if (nrf_cs_cycle_encode_go(&go, frame) != 0) return -1;
    return write_with_retries(coord, node, frame, sizeof(frame), NRF_CS_COORD_GO_TRIES);
}

/* ---- 3. wait ---- */

static bool read_status(coord_t *coord, nrf_cs_node_id_t node, nrf_cs_cycle_status_t *status)
{
    uint8_t frame[NRF_CS_CYCLE_STATUS_LENGTH];
    if (coord->ops->read_node(coord->ops->context, node, frame, sizeof(frame)) != 0) return false;
    if (nrf_cs_cycle_decode_status(frame, sizeof(frame), status) != 0) return false;
    return status->cycle_id == coord->scheduler->cycle_id;
}

static void poll_main(coord_t *coord, nrf_cs_node_id_t main_node)
{
    nrf_cs_cycle_status_t status;
    ++coord->report->polls;
    if (!read_status(coord, main_node, &status)) return;
    coord->report->main_status_seen = true;
    coord->report->main_state = status.state;
    coord->report->main_failure = status.failure;
    if (status.state == NRF_CS_CYCLE_STATE_COMPLETE || status.state == NRF_CS_CYCLE_STATE_FAILED) {
        coord->report->main_complete = true;
    }
    if (coord->ops->progress != NULL) coord->ops->progress(coord->ops->context, status.pairs_done, status.pairs_total);
}

/* Returns true when MAIN reported the end of the cycle, false at the deadline. */
static int wait_for_cycle(coord_t *coord, nrf_cs_node_id_t main_node, uint64_t go_done_ms, uint32_t run_ms)
{
    const uint64_t run_end = go_done_ms + run_ms;
    const uint64_t deadline = run_end + NRF_CS_CYCLE_TOTAL_MARGIN_MS;
    uint64_t next_poll = go_done_ms + NRF_CS_COORD_POLL_PERIOD_MS;

    for (;;) {
        uint64_t now = now_ms(coord);
        if (should_abort(coord)) return NRF_CS_COORD_ERR_ABORTED;
        if (now >= deadline || (coord->report->main_complete && now >= run_end)) return 0;
        if (now >= next_poll) {
            poll_main(coord, main_node);
            now = now_ms(coord);
            next_poll = coord->report->main_complete && now < run_end
                ? run_end : now + (now >= run_end ? NRF_CS_COORD_POLL_FAST_MS : NRF_CS_COORD_POLL_PERIOD_MS);
            continue;
        }
        uint64_t until = next_poll < deadline ? next_poll : deadline;
        uint64_t slice = until > now ? until - now : 0U;
        if (slice > NRF_CS_COORD_SLEEP_SLICE_MS) slice = NRF_CS_COORD_SLEEP_SLICE_MS;
        if (slice == 0U) slice = 1U;
        pause_ms(coord, (uint32_t)slice);
    }
}

/* ---- 4. collect ---- */

static bool read_page(
    coord_t *coord, nrf_cs_node_id_t node, uint8_t page, uint64_t give_up_ms, nrf_cs_cycle_result_t *result)
{
    uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH];
    uint8_t request_frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];

    for (unsigned tries = 0U; tries < NRF_CS_COORD_PAGE_TRIES; ++tries) {
        if (should_abort(coord)) return false;
        if (tries > 0U) {
            if (now_ms(coord) >= give_up_ms) return false;
            pause_ms(coord, NRF_CS_COORD_RETRY_PAUSE_MS);
        }
        const nrf_cs_cycle_result_request_t request = {
            .request_id = next_request_id(coord), .cycle_id = coord->scheduler->cycle_id, .page = page};
        if (nrf_cs_cycle_encode_result_request(&request, request_frame) != 0) return false;
        if (coord->ops->write_node(coord->ops->context, node, request_frame, sizeof(request_frame)) != 0) continue;
        if (coord->ops->read_node(coord->ops->context, node, frame, sizeof(frame)) != 0) continue;
        if (nrf_cs_cycle_decode_result(frame, sizeof(frame), result) != 0) continue;
        if (result->cycle_id != request.cycle_id || result->request_id != request.request_id ||
            result->page != page) {
            continue;
        }
        return true;
    }
    return false;
}

/* Reads all pages of one node; returns true when every record arrived. */
static bool collect_node(coord_t *coord, size_t index, uint64_t give_up_ms, nrf_cs_cycle_collected_t *out)
{
    const nrf_cs_scheduler_t *scheduler = coord->scheduler;
    const nrf_cs_node_id_t node = scheduler->nodes[index];
    nrf_cs_cycle_node_records_t *slot = &out->nodes[index];
    uint8_t expected_total = 0U;
    uint8_t page_count = 1U;

    slot->node = node;
    slot->records = &out->records[out->record_count];
    slot->count = 0U;

    for (uint8_t page = 0U; page < page_count; ++page) {
        nrf_cs_cycle_result_t result;
        if (!read_page(coord, node, page, give_up_ms, &result)) return false;
        if (page == 0U) {
            expected_total = result.record_total;
            page_count = result.page_count;
        } else if (result.record_total != expected_total || result.page_count != page_count) {
            return false;
        }
        for (uint8_t i = 0U; i < result.record_count; ++i) {
            const nrf_cs_cycle_record_t *record = &result.records[i];
            /* Only attempts the node was planned to make count; anything else is noise. */
            if (record->execution >= scheduler->execution_count ||
                scheduler->executions[record->execution].initiator != node) {
                continue;
            }
            if (out->record_count >= NRF_CS_CYCLE_MAX_RECORDS) return false;
            out->records[out->record_count++] = *record;
            ++slot->count;
        }
    }
    return true;
}

static bool release_node(coord_t *coord, nrf_cs_node_id_t node)
{
    uint8_t frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];
    const nrf_cs_cycle_result_request_t release = {
        .request_id = next_request_id(coord),
        .cycle_id = coord->scheduler->cycle_id,
        .page = NRF_CS_CYCLE_RESULT_RELEASE_PAGE,
    };
    if (nrf_cs_cycle_encode_result_request(&release, frame) != 0) return false;
    return write_with_retries(coord, node, frame, sizeof(frame), NRF_CS_COORD_WRITE_TRIES) == 0;
}

/*
 * A node that has let go of its cycle answers a STATUS read with the ordinary status frame. While
 * it still serves a cycle (running, finished and waiting for collection, failed) the reply is a
 * cycle STATUS frame; both kinds share the frame magic, the message type tells them apart. A cycle
 * STATUS frame that is damaged still means the node serves a cycle. A read that fails proves
 * nothing.
 */
static bool node_serves_no_cycle(coord_t *coord, nrf_cs_node_id_t node)
{
    uint8_t frame[NRF_CS_CYCLE_STATUS_LENGTH];
    if (coord->ops->read_node(coord->ops->context, node, frame, sizeof(frame)) != 0) return false;
    return !(frame[0] == (uint8_t)'G' && frame[1] == (uint8_t)'S' && frame[3] == NRF_CS_CYCLE_MESSAGE_STATUS);
}

/*
 * Release that is attempted even while an abort is pending. *sent is set once a release frame was
 * acknowledged on the bus; the result is true only when the node was then read back as idle: an
 * acknowledged write does not prove that the node stopped.
 */
static bool release_node_unconditionally(coord_t *coord, nrf_cs_node_id_t node, bool *sent)
{
    uint8_t frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];
    const nrf_cs_cycle_result_request_t release = {
        .request_id = next_request_id(coord),
        .cycle_id = coord->scheduler->cycle_id,
        .page = NRF_CS_CYCLE_RESULT_RELEASE_PAGE,
    };
    if (nrf_cs_cycle_encode_result_request(&release, frame) != 0) return false;
    for (unsigned i = 0U; i < NRF_CS_COORD_RELEASE_TRIES; ++i) {
        if (coord->ops->write_node(coord->ops->context, node, frame, sizeof(frame)) == 0) {
            *sent = true;
            if (node_serves_no_cycle(coord, node)) return true;
        }
        if (i + 1U < NRF_CS_COORD_RELEASE_TRIES) pause_ms(coord, 20U);
    }
    return false;
}

static int coordinate_cycle(
    const nrf_cs_cycle_coordinator_ops_t *ops,
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t status_node,
    uint32_t request_seed,
    nrf_cs_cycle_collected_t *out,
    nrf_cs_cycle_report_t *report)
{
    coord_t coord = {.ops = ops, .scheduler = scheduler, .report = report, .request_id = request_seed};
    int result = NRF_CS_COORD_OK;

    if (ops == NULL || scheduler == NULL || out == NULL || report == NULL || ops->now_ms == NULL ||
        ops->sleep_ms == NULL || ops->write_node == NULL || ops->read_node == NULL) {
        return NRF_CS_COORD_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memset(report, 0, sizeof(*report));
    if (!scheduler->cycle_started || scheduler->node_count < 2U || scheduler->node_count > NRF_CS_MAX_NODES ||
        scheduler->execution_count == 0U || scheduler->execution_count > NRF_CS_CYCLE_MAX_STEPS ||
        scheduler->policy.max_attempts == 0U || scheduler->policy.pair_timeout_ms == 0U ||
        scheduler->policy.pair_timeout_ms > UINT16_MAX || node_index(scheduler, status_node) == SIZE_MAX) {
        return NRF_CS_COORD_ERR_ARGUMENT;
    }
    out->node_count = scheduler->node_count;
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        out->nodes[i].node = scheduler->nodes[i];
        out->nodes[i].records = out->records;
    }

    const uint32_t window_ms = nrf_cs_cycle_window_ms(
        (uint16_t)scheduler->policy.pair_timeout_ms, normalized_samples(&scheduler->policy),
        scheduler->policy.max_attempts);
    const uint32_t run_ms = nrf_cs_cycle_run_ms((uint8_t)scheduler->execution_count, window_ms);
    report->cycle_id = scheduler->cycle_id;
    report->window_ms = window_ms;
    report->run_ms = run_ms;

    /* 0. A node still running a cycle nobody collected refuses a new plan: free every node first. */
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        if (should_abort(&coord)) return NRF_CS_COORD_ERR_ABORTED;
        bool sent = false;
        const bool confirmed = release_node_unconditionally(&coord, scheduler->nodes[i], &sent);
        if (sent) report->prerelease_sent_mask |= (uint8_t)(1U << i);
        if (confirmed) report->prerelease_mask |= (uint8_t)(1U << i);
    }

    /* 1. Plans. The nodes do nothing until GO, so a failure here leaves everything idle. */
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        const int rc = send_plan(&coord, scheduler->nodes[i]);
        if (rc == NRF_CS_COORD_ERR_ABORTED) return rc;
        if (rc != 0) return NRF_CS_COORD_ERR_PLAN;
        report->plan_mask |= (uint8_t)(1U << i);
    }

    /* 2. GO to everybody with the same request id. Late nodes would shift their windows, so a
     * failed GO is not retried later; that node simply never reports and its executions time out. */
    (void)next_request_id(&coord);
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        const int rc = send_go(&coord, scheduler->nodes[i], run_ms + NRF_CS_CYCLE_TOTAL_MARGIN_MS);
        if (rc == NRF_CS_COORD_ERR_ABORTED) return rc;
        if (rc == 0) report->go_mask |= (uint8_t)(1U << i);
    }
    if (report->go_mask == 0U) return NRF_CS_COORD_ERR_GO;

    /* 3. Silence, with an occasional look at MAIN. */
    const uint64_t go_done_ms = now_ms(&coord);
    result = wait_for_cycle(&coord, status_node, go_done_ms, run_ms);
    report->waited_ms = (uint32_t)(now_ms(&coord) - go_done_ms);
    if (result != 0) return result;

    /* 4. Results. A node that cannot be read costs only its own executions. */
    const uint64_t give_up_ms = go_done_ms + run_ms + NRF_CS_CYCLE_TOTAL_MARGIN_MS + NRF_CS_COORD_COLLECT_GRACE_MS;
    bool partial = false;
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        const nrf_cs_node_id_t node = scheduler->nodes[i];
        if (!node_initiates(scheduler, node) || (report->go_mask & (1U << i)) == 0U) {
            out->nodes[i].records = &out->records[out->record_count];
            if (node_initiates(scheduler, node)) partial = true;
            continue;
        }
        if (collect_node(&coord, i, give_up_ms, out)) {
            report->collected_mask |= (uint8_t)(1U << i);
        } else {
            partial = true;
        }
        if (should_abort(&coord)) return NRF_CS_COORD_ERR_ABORTED;
    }
    for (size_t i = 0U; i < scheduler->node_count; ++i) {
        if (release_node(&coord, scheduler->nodes[i])) report->release_mask |= (uint8_t)(1U << i);
    }
    return partial ? NRF_CS_COORD_PARTIAL : NRF_CS_COORD_OK;
}

int nrf_cs_cycle_coordinate(
    const nrf_cs_cycle_coordinator_ops_t *ops,
    const nrf_cs_scheduler_t *scheduler,
    nrf_cs_node_id_t status_node,
    uint32_t request_seed,
    nrf_cs_cycle_collected_t *out,
    nrf_cs_cycle_report_t *report)
{
    const int result = coordinate_cycle(ops, scheduler, status_node, request_seed, out, report);
    if (result == NRF_CS_COORD_ERR_ABORTED) {
        /* Aborted after validation: nodes may hold a plan or be ranging. Stop them all. */
        coord_t coord = {
            .ops = ops, .scheduler = scheduler, .report = report, .request_id = request_seed ^ 0x5A5A5A5AU};
        for (size_t i = 0U; i < scheduler->node_count; ++i) {
            bool sent = false;
            const bool confirmed = release_node_unconditionally(&coord, scheduler->nodes[i], &sent);
            if (sent) report->abort_release_sent_mask |= (uint8_t)(1U << i);
            if (confirmed) report->abort_release_mask |= (uint8_t)(1U << i);
        }
    }
    return result;
}
