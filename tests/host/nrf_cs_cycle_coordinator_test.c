#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_cycle_coordinator.h"
#include "nrf_cs_cycle_replay.h"
#include "nrf_cs_cycle_runtime.h"

/*
 * The coordinator talks to real nrf_cs_cycle_runtime nodes through a fake I2C bus. Time is virtual:
 * every bus transfer costs a few milliseconds and the nodes are ticked every 50 ms while the
 * coordinator sleeps, the way the main loop of a node does.
 */
#define TICK_MS 50U
#define MAX_LOG 256U

typedef struct {
    nrf_cs_cycle_runtime_t rt;
    nrf_cs_node_id_t id;
    uint8_t reply[NRF_CS_CYCLE_RESULT_LENGTH];
    bool reply_selected;
    unsigned executes;
    unsigned plan_writes;
    /* fault injection */
    unsigned fail_plan_writes; /* drop this many PLAN frames (NACK) */
    bool fail_go;
    unsigned corrupt_result_reads;
    bool never_answers_results;
    bool noise_record;     /* adds a record for an execution this node does not own */
    bool stale_cycle_id;   /* status answers carry another cycle's id */
} sim_node_t;

typedef struct {
    uint64_t at_ms;
    nrf_cs_node_id_t node;
    bool is_write;
    uint8_t type;
} bus_log_t;

static uint64_t g_now;
static uint32_t g_bus_hz = 100000U;
static sim_node_t g_nodes[NRF_CS_MAX_NODES];
static size_t g_node_count;
static bus_log_t g_log[MAX_LOG];
static size_t g_log_count;
static bool g_main_unreadable;
static bool g_main_early_complete;
static unsigned g_noise_injected;
static unsigned g_abort_after_polls;
static nrf_cs_node_id_t g_main;
static unsigned g_always_fail_mask; /* executions that never work */

static uint64_t fake_now(void *context) { (void)context; return g_now; }

/* Address and data bytes each need nine clock pulses; round up to virtual milliseconds. */
static uint32_t transfer_ms(size_t length)
{
    return (uint32_t)(((length + 1U) * 9000U + g_bus_hz - 1U) / g_bus_hz);
}

static nrf_cs_measurement_t scripted(unsigned execution, unsigned attempt)
{
    nrf_cs_measurement_t m = {0};
    if ((g_always_fail_mask & (1U << execution)) != 0U) {
        m.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
        return m;
    }
    if (execution % 3U == 0U && attempt == 1U) {
        m.outcome = NRF_CS_OUTCOME_TIMEOUT;
        return m;
    }
    m.outcome = NRF_CS_OUTCOME_SUCCESS;
    m.distance_mm = (int32_t)(1000U + 10U * execution + 3U * attempt);
    m.confidence = 800U;
    m.provider_quality_flags = (uint16_t)(execution & 0x3U);
    return m;
}

static void tick_nodes(void)
{
    for (size_t n = 0U; n < g_node_count; ++n) {
        sim_node_t *sim = &g_nodes[n];
        nrf_cs_step_t step;
        nrf_cs_cycle_runtime_tick(&sim->rt);
        if (nrf_cs_cycle_runtime_take_start(&sim->rt, &step)) {
            ++sim->executes;
            nrf_cs_cycle_runtime_start_result(&sim->rt, &step, 0);
            const nrf_cs_measurement_t m = scripted(sim->rt.window, step.attempt_number);
            assert(nrf_cs_cycle_runtime_step_done(&sim->rt, &step, &m) == 0);
        }
    }
}

static void fake_sleep(void *context, uint32_t ms)
{
    (void)context;
    while (ms > 0U) {
        const uint32_t step = ms < TICK_MS ? ms : TICK_MS;
        g_now += step;
        ms -= step;
        tick_nodes();
    }
}

static sim_node_t *find(nrf_cs_node_id_t id)
{
    for (size_t n = 0U; n < g_node_count; ++n) {
        if (g_nodes[n].id == id) return &g_nodes[n];
    }
    return NULL;
}

static void log_op(nrf_cs_node_id_t node, bool is_write, uint8_t type)
{
    if (g_log_count < MAX_LOG) g_log[g_log_count++] = (bus_log_t){g_now, node, is_write, type};
}

static int fake_write(void *context, nrf_cs_node_id_t node, const uint8_t *frame, size_t length)
{
    (void)context;
    sim_node_t *sim = find(node);
    g_now += transfer_ms(length);
    log_op(node, true, length > 3U ? frame[3] : 0U);
    if (sim == NULL || length < 4U) return -1;
    sim->reply_selected = false;
    switch (frame[3]) {
    case NRF_CS_CYCLE_MESSAGE_PLAN: {
        nrf_cs_cycle_plan_t plan;
        ++sim->plan_writes;
        if (sim->fail_plan_writes > 0U) { --sim->fail_plan_writes; return -1; }
        if (nrf_cs_cycle_decode_plan(frame, length, &plan) == 0) (void)nrf_cs_cycle_runtime_accept_plan(&sim->rt, node, &plan);
        return 0;
    }
    case NRF_CS_CYCLE_MESSAGE_PLAN_STEPS: {
        nrf_cs_cycle_plan_steps_t chunk;
        if (nrf_cs_cycle_decode_plan_steps(frame, length, &chunk) == 0) (void)nrf_cs_cycle_runtime_accept_steps(&sim->rt, &chunk);
        return 0;
    }
    case NRF_CS_CYCLE_MESSAGE_GO: {
        nrf_cs_cycle_go_t go;
        if (sim->fail_go) return -1;
        if (nrf_cs_cycle_decode_go(frame, length, &go) == 0) (void)nrf_cs_cycle_runtime_go(&sim->rt, &go);
        return 0;
    }
    case NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST: {
        nrf_cs_cycle_result_request_t request;
        nrf_cs_cycle_result_t page;
        if (nrf_cs_cycle_decode_result_request(frame, length, &request) != 0) return 0;
        if (request.page == NRF_CS_CYCLE_RESULT_RELEASE_PAGE) {
            nrf_cs_cycle_runtime_release(&sim->rt);
        } else if (nrf_cs_cycle_runtime_result_page(&sim->rt, request.request_id, request.page, &page) == 0 &&
                   page.cycle_id == request.cycle_id && !sim->never_answers_results) {
            /* One extra record at the end of the last page; every page then reports the larger total. */
            if (sim->noise_record && (page.record_total % NRF_CS_CYCLE_RESULTS_PER_PAGE) != 0U) {
                ++page.record_total;
                if (page.page == page.page_count - 1U) {
                    for (uint8_t e = 0U; e < sim->rt.step_count; ++e) {
                        if (sim->rt.steps[e].initiator == node) continue;
                        page.records[page.record_count++] = (nrf_cs_cycle_record_t){
                            .execution = e, .attempt = 1U, .outcome = NRF_CS_OUTCOME_SUCCESS,
                            .distance_mm = 99999, .confidence = 900U};
                        ++g_noise_injected;
                        break;
                    }
                }
            }
            assert(nrf_cs_cycle_encode_result(&page, sim->reply) == 0);
            if (sim->corrupt_result_reads > 0U) { --sim->corrupt_result_reads; sim->reply[30] ^= 0x5AU; }
            sim->reply_selected = true;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int fake_read(void *context, nrf_cs_node_id_t node, uint8_t *frame, size_t length)
{
    (void)context;
    sim_node_t *sim = find(node);
    g_now += transfer_ms(length);
    log_op(node, false, 0U);
    if (sim == NULL) return -1;
    if (node == g_main && g_main_unreadable && length == NRF_CS_CYCLE_STATUS_LENGTH) return -1;
    memset(frame, 0, length);
    if (sim->reply_selected && length == NRF_CS_CYCLE_RESULT_LENGTH) {
        memcpy(frame, sim->reply, length);
        sim->reply_selected = false;
        return 0;
    }
    if (nrf_cs_cycle_runtime_reply_active(&sim->rt) && length == NRF_CS_CYCLE_STATUS_LENGTH) {
        nrf_cs_cycle_status_t status;
        nrf_cs_cycle_runtime_status(&sim->rt, 0U, &status);
        if (sim->stale_cycle_id) status.cycle_id += 1U;
        if (node == g_main && g_main_early_complete) {
            status.state = NRF_CS_CYCLE_STATE_COMPLETE;
            status.pairs_done = status.pairs_total;
        }
        assert(nrf_cs_cycle_encode_status(&status, frame) == 0);
        return 0;
    }
    frame[3] = 0x80U; /* the legacy STATUS: not a cycle frame */
    return 0;
}

static bool fake_should_abort(void *context)
{
    (void)context;
    if (g_abort_after_polls == 0U) return false;
    unsigned polls = 0U;
    for (size_t i = 0U; i < g_log_count; ++i) {
        if (!g_log[i].is_write && g_log[i].node == g_main) ++polls;
    }
    return polls >= g_abort_after_polls;
}

static const nrf_cs_cycle_coordinator_ops_t g_ops = {
    .now_ms = fake_now, .sleep_ms = fake_sleep, .write_node = fake_write, .read_node = fake_read,
    .should_abort = fake_should_abort,
};

static void setup(size_t node_count, nrf_cs_node_id_t *ids)
{
    const nrf_cs_cycle_runtime_ops_t ops = {.now_ms = fake_now};
    memset(g_nodes, 0, sizeof(g_nodes));
    g_node_count = node_count;
    g_log_count = 0U;
    g_now = 100000U;
    g_main_unreadable = false;
    g_main_early_complete = false;
    g_abort_after_polls = 0U;
    g_always_fail_mask = 0U;
    for (size_t n = 0U; n < node_count; ++n) {
        ids[n] = (nrf_cs_node_id_t)(10U * (n + 1U));
        g_nodes[n].id = ids[n];
        nrf_cs_cycle_runtime_init(&g_nodes[n].rt, &ops);
    }
    g_main = ids[0];
}

static const nrf_cs_policy_t k_policy = {
    .pair_timeout_ms = 500U, .max_attempts = 3U, .min_confidence = 500U, .samples_per_pair = 3U, .full_rotation = true,
};

static void start_scheduler(nrf_cs_scheduler_t *scheduler, const nrf_cs_node_id_t *ids, size_t count, const nrf_cs_policy_t *policy)
{
    assert(nrf_cs_scheduler_init(scheduler, policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(scheduler, ids, count) == NRF_CS_OK);
}

/* What the ESP32 would have measured by driving every step itself. */
static void run_live(nrf_cs_scheduler_t *scheduler)
{
    nrf_cs_step_t step;
    for (;;) {
        const nrf_cs_result_t next = nrf_cs_scheduler_next_step(scheduler, &step);
        if (next == NRF_CS_CYCLE_COMPLETE) return;
        assert(next == NRF_CS_STEP_READY);
        const nrf_cs_measurement_t m = scripted((unsigned)scheduler->active_execution_index, step.attempt_number);
        assert(nrf_cs_scheduler_complete_step(scheduler, &step, &m) == NRF_CS_OK);
    }
}

static void assert_same_pairs(const nrf_cs_scheduler_t *a, const nrf_cs_scheduler_t *b)
{
    assert(nrf_cs_scheduler_pair_count(a) == nrf_cs_scheduler_pair_count(b));
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(a); ++i) {
        const nrf_cs_pair_result_t *x = nrf_cs_scheduler_pair_at(a, i);
        const nrf_cs_pair_result_t *y = nrf_cs_scheduler_pair_at(b, i);
        assert(x->node_a == y->node_a && x->node_b == y->node_b);
        assert(x->state == y->state && x->distance_mm == y->distance_mm && x->confidence == y->confidence);
        assert(x->attempts == y->attempts && x->valid_samples == y->valid_samples);
        assert(x->last_failure == y->last_failure && x->single_direction == y->single_direction);
    }
}

static unsigned count_ops(uint64_t from_ms, uint64_t to_ms, bool writes, nrf_cs_node_id_t only_not)
{
    unsigned n = 0U;
    for (size_t i = 0U; i < g_log_count; ++i) {
        if (g_log[i].at_ms < from_ms || g_log[i].at_ms >= to_ms) continue;
        if (g_log[i].is_write != writes) continue;
        if (only_not != 0U && g_log[i].node == only_not) continue;
        ++n;
    }
    return n;
}

static uint64_t last_go_end(void)
{
    uint64_t last = 0U;
    for (size_t i = 0U; i < g_log_count; ++i) {
        if (g_log[i].is_write && g_log[i].type == NRF_CS_CYCLE_MESSAGE_GO) last = g_log[i].at_ms;
    }
    return last;
}

static void test_budget_and_arguments(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];
    nrf_cs_policy_t bad = k_policy;

    /* 4 nodes, full rotation: 12 executions of (300 + 5 * 650) ms, plus margins. */
    assert(nrf_cs_cycle_coordinator_budget_ms(&k_policy, 12U, 4U) ==
           12U * 3550U + NRF_CS_CYCLE_TOTAL_MARGIN_MS + NRF_CS_COORD_COLLECT_GRACE_MS + 4U * 2500U);
    assert(nrf_cs_cycle_coordinator_budget_ms(NULL, 12U, 4U) == 0U);
    bad.max_attempts = 0U;
    assert(nrf_cs_cycle_coordinator_budget_ms(&bad, 12U, 4U) == 0U);

    setup(4U, ids);
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(NULL, &scheduler, g_main, 1U, &out, &report) == NRF_CS_COORD_ERR_ARGUMENT);
    assert(nrf_cs_cycle_coordinate(&g_ops, NULL, g_main, 1U, &out, &report) == NRF_CS_COORD_ERR_ARGUMENT);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, 99U, 1U, &out, &report) == NRF_CS_COORD_ERR_ARGUMENT);
    nrf_cs_scheduler_t unstarted;
    assert(nrf_cs_scheduler_init(&unstarted, &k_policy) == NRF_CS_OK);
    assert(nrf_cs_cycle_coordinate(&g_ops, &unstarted, g_main, 1U, &out, &report) == NRF_CS_COORD_ERR_ARGUMENT);
    assert(g_log_count == 0U); /* nothing reached the bus */
}

static void test_happy_path_matches_live_scheduler(void)
{
    nrf_cs_scheduler_t scheduler, live;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_always_fail_mask = 1U << 5; /* one execution never works */
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    start_scheduler(&live, ids, 4U, &k_policy);

    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(report.plan_mask == 0x0FU && report.go_mask == 0x0FU && report.collected_mask == 0x0FU);
    assert(report.release_mask == 0x0FU);
    assert(report.window_ms == 3550U && report.run_ms == 12U * 3550U);
    assert(report.main_complete && report.main_state == NRF_CS_CYCLE_STATE_COMPLETE);

    /* Replaying the node records gives the live scheduler's pair results. */
    assert(nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count) == 0);
    run_live(&live);
    assert(nrf_cs_scheduler_is_complete(&scheduler) && nrf_cs_scheduler_is_complete(&live));
    assert_same_pairs(&scheduler, &live);
    /* The failing execution really cost its pair: the sample set is smaller than a clean pair's. */
    bool saw_failed = false;
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        if (nrf_cs_scheduler_pair_at(&scheduler, i)->single_direction) saw_failed = true;
    }
    assert(saw_failed);

    /* Every node started the cycle once, and every node was released afterwards. */
    for (size_t n = 0U; n < g_node_count; ++n) {
        assert(g_nodes[n].executes > 0U);
        assert(!nrf_cs_cycle_runtime_reply_active(&g_nodes[n].rt));
    }
    /* Every node got a plan before anything was started. */
    for (size_t n = 0U; n < g_node_count; ++n) assert(g_nodes[n].plan_writes == 1U);
}

static void test_bus_is_silent_while_nodes_range(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);

    const uint64_t go_end = last_go_end();
    const uint64_t run_end = go_end + report.run_ms;
    /* Nothing is written and only MAIN is read between the last GO and the end of the run. */
    assert(count_ops(go_end + 1U, run_end, true, 0U) == 0U);
    assert(count_ops(go_end + 1U, run_end, false, g_main) == 0U);
    /* MAIN is polled sparsely: at most one read per poll period. */
    const unsigned main_reads = count_ops(go_end + 1U, run_end, false, 0U);
    assert(main_reads >= 1U);
    assert(main_reads <= report.run_ms / 5000U + 1U);
    uint64_t previous = 0U;
    for (size_t i = 0U; i < g_log_count; ++i) {
        if (g_log[i].at_ms <= go_end || g_log[i].at_ms >= run_end) continue;
        if (previous != 0U) assert(g_log[i].at_ms - previous >= 5000U);
        previous = g_log[i].at_ms;
    }
    /* Once the plan went out and GO is done, the run is longer than a handful of polls. */
    assert(report.polls >= 2U);
}

static void test_early_main_completion_keeps_bus_quiet_until_all_windows_end(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];
    setup(4U, ids);
    g_main_early_complete = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    const uint64_t go_end = last_go_end();
    assert(report.waited_ms >= report.run_ms);
    assert(count_ops(go_end + 1U, go_end + report.run_ms, true, 0U) == 0U);
    assert(count_ops(go_end + 1U, go_end + report.run_ms, false, g_main) == 0U);
}

static void test_plan_failure_starts_nothing(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_nodes[2].fail_plan_writes = 100U; /* the third node never takes its plan */
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_ERR_PLAN);
    assert(report.plan_mask == 0x03U && report.go_mask == 0U);
    for (size_t i = 0U; i < g_log_count; ++i) {
        assert(!(g_log[i].is_write && g_log[i].type == NRF_CS_CYCLE_MESSAGE_GO));
    }
    for (size_t n = 0U; n < g_node_count; ++n) assert(!nrf_cs_cycle_runtime_running(&g_nodes[n].rt));
    assert(g_nodes[2].plan_writes == NRF_CS_COORD_WRITE_TRIES);

    /* A single dropped frame is retried and the cycle goes on. */
    setup(4U, ids);
    g_nodes[1].fail_plan_writes = 1U;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(g_nodes[1].plan_writes == 2U);
}

static void test_go_failure_costs_only_that_node(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_nodes[3].fail_go = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_PARTIAL);
    assert(report.go_mask == 0x07U && report.collected_mask == 0x07U);
    assert(g_nodes[3].executes == 0U);
    /* The silent node's executions come out as timeouts; the others keep their samples. */
    const int missing = nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count);
    assert(missing > 0);
    bool good_pair = false;
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_scheduler_pair_at(&scheduler, i);
        if (pair->node_a != ids[3] && pair->node_b != ids[3]) {
            assert(pair->state == NRF_CS_PAIR_VALID);
            good_pair = true;
        }
    }
    assert(good_pair);

    /* Nobody started at all: the coordinator gives up before waiting. */
    setup(4U, ids);
    for (size_t n = 0U; n < 4U; ++n) g_nodes[n].fail_go = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    const uint64_t before = g_now;
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_ERR_GO);
    assert(g_now - before < 5000U);
}

static void test_flaky_result_reads_are_retried(void)
{
    nrf_cs_scheduler_t scheduler, live;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_nodes[1].corrupt_result_reads = 2U;
    g_nodes[2].corrupt_result_reads = 4U;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    start_scheduler(&live, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(report.collected_mask == 0x0FU);
    assert(nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count) == 0);
    run_live(&live);
    assert_same_pairs(&scheduler, &live);

    /* A node that never answers costs its executions, not the whole cycle. */
    setup(4U, ids);
    g_nodes[2].never_answers_results = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_PARTIAL);
    assert(report.collected_mask == 0x0BU);
    assert((report.release_mask & 0x04U) != 0U); /* still released */
}

static void test_unreadable_main_still_collects_after_deadline(void)
{
    nrf_cs_scheduler_t scheduler, live;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_main_unreadable = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    start_scheduler(&live, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(!report.main_status_seen && !report.main_complete);
    assert(report.waited_ms >= report.run_ms + NRF_CS_CYCLE_TOTAL_MARGIN_MS);
    assert(nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count) == 0);
    run_live(&live);
    assert_same_pairs(&scheduler, &live);
}

static void test_abort_stops_waiting_and_leaves_nodes_alone(void)
{
    nrf_cs_scheduler_t scheduler;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(4U, ids);
    g_abort_after_polls = 2U;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_ERR_ABORTED);
    assert(report.waited_ms < report.run_ms);
    assert(report.collected_mask == 0U && report.release_mask == 0U);
}

static void test_noise_records_and_foreign_status_are_ignored(void)
{
    nrf_cs_scheduler_t scheduler, live;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    /* A record for an execution another node owns must not reach the replay. */
    setup(4U, ids);
    g_noise_injected = 0U;
    for (size_t n = 0U; n < 4U; ++n) g_nodes[n].noise_record = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    start_scheduler(&live, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(g_noise_injected > 0U);
    size_t own = 0U;
    for (size_t i = 0U; i < out.node_count; ++i) {
        for (size_t r = 0U; r < out.nodes[i].count; ++r) {
            assert(scheduler.executions[out.nodes[i].records[r].execution].initiator == out.nodes[i].node);
            assert(out.nodes[i].records[r].distance_mm != 99999);
            ++own;
        }
    }
    assert(own == out.record_count);
    assert(nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count) == 0);
    run_live(&live);
    assert_same_pairs(&scheduler, &live);

    /* A status that belongs to another cycle is not MAIN's answer. */
    setup(4U, ids);
    g_nodes[0].stale_cycle_id = true;
    start_scheduler(&scheduler, ids, 4U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(!report.main_status_seen && !report.main_complete);
    assert(report.waited_ms >= report.run_ms + NRF_CS_CYCLE_TOTAL_MARGIN_MS);
}

static void test_five_nodes_all_pairs(void)
{
    nrf_cs_scheduler_t scheduler, live;
    nrf_cs_cycle_collected_t out;
    nrf_cs_cycle_report_t report;
    nrf_cs_node_id_t ids[NRF_CS_MAX_NODES];

    setup(5U, ids);
    start_scheduler(&scheduler, ids, 5U, &k_policy);
    start_scheduler(&live, ids, 5U, &k_policy);
    assert(nrf_cs_cycle_coordinate(&g_ops, &scheduler, g_main, 100U, &out, &report) == NRF_CS_COORD_OK);
    assert(scheduler.execution_count == 20U && report.run_ms == 20U * 3550U);
    assert(report.collected_mask == 0x1FU);
    assert(nrf_cs_cycle_replay(&scheduler, out.nodes, out.node_count) == 0);
    run_live(&live);
    assert_same_pairs(&scheduler, &live);
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&scheduler); ++i) {
        assert(nrf_cs_scheduler_pair_at(&scheduler, i)->state == NRF_CS_PAIR_VALID);
    }
}

int main(void)
{
    test_budget_and_arguments();
    const uint32_t rates[] = {25000U, 50000U, 100000U};
    for (size_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        g_bus_hz = rates[i];
        test_happy_path_matches_live_scheduler();
        test_bus_is_silent_while_nodes_range();
        test_early_main_completion_keeps_bus_quiet_until_all_windows_end();
        test_flaky_result_reads_are_retried();
        test_noise_records_and_foreign_status_are_ignored();
    }
    test_plan_failure_starts_nothing();
    test_go_failure_costs_only_that_node();
    test_unreadable_main_still_collects_after_deadline();
    test_abort_stops_waiting_and_leaves_nodes_alone();
    test_five_nodes_all_pairs();
    puts("nrf_cs_cycle_coordinator tests passed");
    return 0;
}
