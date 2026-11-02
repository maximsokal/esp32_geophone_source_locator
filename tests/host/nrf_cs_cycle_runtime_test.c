#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_cycle_replay.h"
#include "nrf_cs_cycle_runtime.h"

#define MAX_SIM_NODES NRF_CS_MAX_NODES

static uint64_t g_now;

typedef struct {
    nrf_cs_cycle_runtime_t rt;
    nrf_cs_node_id_t id;
    bool active;
    uint64_t due_ms;
    nrf_cs_step_t step;
    nrf_cs_measurement_t outcome;
    unsigned latency_ms;      /* time an attempt needs; above the timeout it never finishes in time */
    int refuse;               /* start result: 0 accept, >0 busy, <0 refuse */
    int busy_first;           /* answer busy (>0) this many times before accepting */
    bool always_succeed;      /* ignore the script: every attempt succeeds */
    unsigned executes;
    unsigned aborts;
    uint64_t last_end_ms;     /* when the previous attempt ended (finished, refused or aborted) */
} sim_node_t;

static uint64_t fake_now(void *context) { (void)context; return g_now; }

/* Deterministic outcome of attempt `attempt` of execution `execution`, shared by both drivers. */
static nrf_cs_measurement_t scripted(unsigned execution, unsigned attempt)
{
    nrf_cs_measurement_t m = {0};
    if (execution % 3U == 0U && attempt == 1U) {
        m.outcome = NRF_CS_OUTCOME_TIMEOUT;
        return m;
    }
    if (execution % 5U == 2U) {
        m.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR; /* never works: the retry budget must end it */
        return m;
    }
    if (execution % 4U == 1U && attempt == 2U) {
        m.outcome = NRF_CS_OUTCOME_SUCCESS; /* accepted by the BLE side, rejected by min_confidence */
        m.distance_mm = 5000; m.confidence = 100U;
        return m;
    }
    m.outcome = NRF_CS_OUTCOME_SUCCESS;
    m.distance_mm = (int32_t)(1000U + 10U * execution + 3U * attempt);
    m.confidence = 800U;
    m.provider_quality_flags = (uint16_t)(execution & 0x3U);
    return m;
}

static int sim_execute(sim_node_t *sim, const nrf_cs_step_t *step)
{
    ++sim->executes;
    if (sim->busy_first > 0) {
        --sim->busy_first;
        return 1; /* BLE busy: not an attempt */
    }
    /* BLE needs a pause after an attempt to clean up and advertise again. */
    assert(sim->last_end_ms == 0U || g_now >= sim->last_end_ms + NRF_CS_CYCLE_ATTEMPT_GAP_MS);
    if (sim->refuse != 0) {
        sim->last_end_ms = g_now;
        return sim->refuse;
    }
    assert(!sim->active);
    assert(step->initiator == sim->id);
    /* An attempt starts only after the GO skew margin and only when a whole timeout still fits. */
    const uint64_t offset = g_now - sim->rt.t0_ms - (uint64_t)sim->rt.window * sim->rt.window_ms;
    assert(offset >= NRF_CS_CYCLE_GO_SKEW_MS);
    assert(sim->rt.window_ms - offset >= sim->rt.pair_timeout_ms);
    assert(sim->rt.steps[sim->rt.window].initiator == sim->id);
    sim->active = true;
    sim->step = *step;
    sim->due_ms = g_now + sim->latency_ms;
    sim->outcome = scripted(sim->rt.window, step->attempt_number);
    if (sim->always_succeed) {
        sim->outcome = (nrf_cs_measurement_t){.outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1500, .confidence = 900U};
    }
    return 0;
}

static void sim_abort(void *context)
{
    sim_node_t *sim = context;
    ++sim->aborts;
    sim->active = false;
    sim->last_end_ms = g_now;
}

/* One tick as the firmware glue does it: prepare, take the attempt, start it, report how that went. */
static void sim_tick(sim_node_t *sim)
{
    nrf_cs_step_t step;
    nrf_cs_cycle_runtime_tick(&sim->rt);
    if (nrf_cs_cycle_runtime_take_start(&sim->rt, &step)) {
        assert(!nrf_cs_cycle_runtime_take_start(&sim->rt, &step)); /* an attempt is handed out once */
        const int result = sim_execute(sim, &step);
        nrf_cs_cycle_runtime_start_result(&sim->rt, &step, result);
    }
}

static nrf_cs_policy_t make_policy(void)
{
    return (nrf_cs_policy_t){
        .pair_timeout_ms = 500U, .max_attempts = 3U, .min_confidence = 500U,
        .samples_per_pair = 3U, .full_rotation = true,
    };
}

static void init_sim(sim_node_t *sim, nrf_cs_node_id_t id)
{
    const nrf_cs_cycle_runtime_ops_t ops = {.context = sim, .now_ms = fake_now, .abort_attempt = sim_abort};
    memset(sim, 0, sizeof(*sim));
    sim->id = id;
    sim->latency_ms = 300U;
    nrf_cs_cycle_runtime_init(&sim->rt, &ops);
}

/* What the ESP32 does before the quiet time: build the plan from a scheduler cycle and hand it out. */
static size_t send_plan(sim_node_t *sims, size_t count, const nrf_cs_scheduler_t *planner, uint32_t request_base)
{
    for (size_t n = 0U; n < count; ++n) {
        nrf_cs_cycle_plan_t plan = {
            .request_id = request_base + (uint32_t)n, .cycle_id = planner->cycle_id,
            .membership_generation = (uint16_t)planner->cycle_membership_generation,
            .pair_timeout_ms = (uint16_t)planner->policy.pair_timeout_ms,
            .max_attempts = planner->policy.max_attempts, .samples_per_pair = planner->policy.samples_per_pair,
            .min_confidence = planner->policy.min_confidence, .full_rotation = planner->policy.full_rotation,
            .step_count = (uint8_t)planner->execution_count, .node_count = (uint8_t)planner->node_count,
            .self_node = sims[n].id,
        };
        assert(nrf_cs_cycle_runtime_accept_plan(&sims[n].rt, sims[n].id, &plan) == 0);
        assert(!nrf_cs_cycle_runtime_plan_ready(&sims[n].rt));
        for (size_t first = 0U; first < planner->execution_count; first += NRF_CS_CYCLE_STEPS_PER_CHUNK) {
            nrf_cs_cycle_plan_steps_t chunk = {.request_id = request_base + 100U, .cycle_id = planner->cycle_id, .first_step = (uint8_t)first};
            const size_t left = planner->execution_count - first;
            chunk.step_count = (uint8_t)(left < NRF_CS_CYCLE_STEPS_PER_CHUNK ? left : NRF_CS_CYCLE_STEPS_PER_CHUNK);
            for (uint8_t i = 0U; i < chunk.step_count; ++i) {
                chunk.steps[i].initiator = planner->executions[first + i].initiator;
                chunk.steps[i].reflector = planner->executions[first + i].reflector;
            }
            assert(nrf_cs_cycle_runtime_accept_steps(&sims[n].rt, &chunk) == 0);
        }
        assert(nrf_cs_cycle_runtime_plan_ready(&sims[n].rt));
    }
    return planner->execution_count;
}

/* GO reaches the nodes 20 ms apart; then simulated time runs until every node finished. */
static void run_cycle(sim_node_t *sims, size_t count, uint32_t cycle_id)
{
    const nrf_cs_policy_t policy = make_policy();
    const uint32_t window = nrf_cs_cycle_window_ms((uint16_t)policy.pair_timeout_ms, policy.samples_per_pair, policy.max_attempts);
    const uint64_t start = g_now;
    const uint32_t run_ms = nrf_cs_cycle_run_ms(sims[0].rt.step_count, window);
    bool sent[MAX_SIM_NODES] = {false};
    size_t completed = 0U;

    while (completed < count) {
        for (size_t n = 0U; n < count; ++n) {
            if (!sent[n] && g_now >= start + 20U * n) {
                const nrf_cs_cycle_go_t go = {.request_id = 900U + (uint32_t)n, .cycle_id = cycle_id, .total_timeout_ms = run_ms + NRF_CS_CYCLE_TOTAL_MARGIN_MS};
                assert(nrf_cs_cycle_runtime_go(&sims[n].rt, &go) == 0);
                assert(nrf_cs_cycle_runtime_go(&sims[n].rt, &go) == 0); /* a repeated GO changes nothing */
                sent[n] = true;
            }
        }
        for (size_t n = 0U; n < count; ++n) {
            if (!sent[n]) continue;
            sim_node_t *sim = &sims[n];
            if (sim->active && g_now >= sim->due_ms) {
                sim->active = false;
                sim->last_end_ms = g_now;
                assert(nrf_cs_cycle_runtime_step_done(&sim->rt, &sim->step, &sim->outcome) == 0);
            }
            sim_tick(sim);
        }
        completed = 0U;
        for (size_t n = 0U; n < count; ++n) {
            if (sims[n].rt.state == NRF_CS_CYCLE_STATE_COMPLETE) ++completed;
        }
        g_now += 25U;
        assert(g_now - start < (uint64_t)run_ms + 10000U); /* the cycle must end by itself */
    }
}

/* The reference: the ESP32 drives every step itself, like the old authority did. */
static void run_live(nrf_cs_scheduler_t *scheduler)
{
    nrf_cs_step_t step;
    while (nrf_cs_scheduler_next_step(scheduler, &step) == NRF_CS_STEP_READY) {
        const nrf_cs_measurement_t m = scripted((unsigned)scheduler->active_execution_index, step.attempt_number);
        assert(nrf_cs_scheduler_complete_step(scheduler, &step, &m) == NRF_CS_OK);
    }
    assert(nrf_cs_scheduler_is_complete(scheduler));
}

static void assert_same_pairs(const nrf_cs_scheduler_t *a, const nrf_cs_scheduler_t *b)
{
    assert(nrf_cs_scheduler_pair_count(a) == nrf_cs_scheduler_pair_count(b));
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(a); ++i) {
        const nrf_cs_pair_result_t *x = nrf_cs_scheduler_pair_at(a, i);
        const nrf_cs_pair_result_t *y = nrf_cs_scheduler_pair_at(b, i);
        assert(x->node_a == y->node_a && x->node_b == y->node_b && x->state == y->state);
        assert(x->distance_mm == y->distance_mm && x->confidence == y->confidence);
        assert(x->spread_mm == y->spread_mm && x->valid_samples == y->valid_samples);
        assert(x->attempts == y->attempts && x->last_failure == y->last_failure);
        assert(x->provider_quality_flags == y->provider_quality_flags);
        assert(x->scheduler_quality_flags == y->scheduler_quality_flags);
        assert(x->rejected_samples == y->rejected_samples && x->single_direction == y->single_direction);
        assert(x->direction_disagreement_mm == y->direction_disagreement_mm);
    }
}

/* Reads every node's pages the way the ESP32 will, then replays them. */
static size_t collect_records(sim_node_t *sim, nrf_cs_cycle_record_t *out, size_t capacity)
{
    nrf_cs_cycle_result_t page;
    size_t count = 0U;
    if (nrf_cs_cycle_runtime_result_page(&sim->rt, 5U, 0U, &page) != 0) return 0U;
    const uint8_t pages = page.page_count;
    for (uint8_t p = 0U; p < pages; ++p) {
        uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH];
        nrf_cs_cycle_result_t decoded;
        assert(nrf_cs_cycle_runtime_result_page(&sim->rt, 5U, p, &page) == 0);
        assert(nrf_cs_cycle_encode_result(&page, frame) == 0);          /* goes through the real wire format */
        assert(nrf_cs_cycle_decode_result(frame, sizeof(frame), &decoded) == 0);
        for (uint8_t i = 0U; i < decoded.record_count; ++i) {
            assert(count < capacity);
            out[count++] = decoded.records[i];
        }
    }
    assert(count == sim->rt.record_count);
    return count;
}

static void test_autonomous_cycle_matches_live_scheduler(size_t node_count)
{
    static const nrf_cs_node_id_t ids[] = {0U, 1U, 2U, 3U, 4U};
    const nrf_cs_policy_t policy = make_policy();
    nrf_cs_scheduler_t planner, live, replay;
    sim_node_t sims[MAX_SIM_NODES];

    g_now = 1000U;
    assert(nrf_cs_scheduler_init(&planner, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&planner, ids, node_count) == NRF_CS_OK);
    assert(nrf_cs_scheduler_init(&live, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&live, ids, node_count) == NRF_CS_OK);
    assert(nrf_cs_scheduler_init(&replay, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&replay, ids, node_count) == NRF_CS_OK);
    assert(live.cycle_id == planner.cycle_id && replay.cycle_id == planner.cycle_id);

    for (size_t n = 0U; n < node_count; ++n) init_sim(&sims[n], ids[n]);
    assert(send_plan(sims, node_count, &planner, 10U) == planner.execution_count);
    run_cycle(sims, node_count, planner.cycle_id);

    static nrf_cs_cycle_record_t storage[MAX_SIM_NODES][NRF_CS_CYCLE_MAX_RECORDS];
    nrf_cs_cycle_node_records_t nodes[MAX_SIM_NODES];
    size_t total_records = 0U;
    for (size_t n = 0U; n < node_count; ++n) {
        nodes[n].node = ids[n];
        nodes[n].records = storage[n];
        nodes[n].count = collect_records(&sims[n], storage[n], NRF_CS_CYCLE_MAX_RECORDS);
        total_records += nodes[n].count;
        assert(sims[n].aborts == 0U);          /* every attempt finished inside its window */
    }
    assert(total_records > planner.execution_count);

    run_live(&live);
    assert(nrf_cs_cycle_replay(&replay, nodes, node_count) == 0); /* no attempt is missing */
    assert(nrf_cs_scheduler_is_complete(&replay));
    assert_same_pairs(&live, &replay);

    /* Each node made exactly the attempts the scheduler would have issued: no more, no fewer. */
    for (size_t e = 0U; e < planner.execution_count; ++e) {
        const nrf_cs_execution_t *reference = &live.executions[e];
        size_t made = 0U;
        for (size_t n = 0U; n < node_count; ++n) {
            for (size_t i = 0U; i < nodes[n].count; ++i) {
                if (nodes[n].records[i].execution == e) ++made;
            }
        }
        assert(made == reference->attempts);
    }

    /* The windows ran in plan order: each record belongs to the execution its node initiates. */
    for (size_t n = 0U; n < node_count; ++n) {
        for (size_t i = 0U; i < nodes[n].count; ++i) {
            assert(planner.executions[nodes[n].records[i].execution].initiator == ids[n]);
        }
    }
    /* Status after the cycle: complete, every window counted. */
    nrf_cs_cycle_status_t status;
    nrf_cs_cycle_runtime_status(&sims[0].rt, 0U, &status);
    assert(status.state == NRF_CS_CYCLE_STATE_COMPLETE && status.pairs_done == status.pairs_total);
    assert(status.pairs_total == planner.execution_count && status.failure == NRF_CS_CYCLE_FAILURE_NONE);
    assert(nrf_cs_cycle_runtime_reply_active(&sims[0].rt));
    nrf_cs_cycle_runtime_release(&sims[0].rt);
    assert(!nrf_cs_cycle_runtime_reply_active(&sims[0].rt));
    nrf_cs_cycle_runtime_status(&sims[0].rt, 0U, &status);
    assert(status.state == NRF_CS_CYCLE_STATE_IDLE && status.cycle_id == 0U);

    /* A cycle nobody released frees itself, so normal STATUS reads come back on their own. */
    assert(nrf_cs_cycle_runtime_reply_active(&sims[1].rt));
    g_now = sims[1].rt.finished_ms + NRF_CS_CYCLE_REPLY_HOLD_MS - 1U;
    nrf_cs_cycle_runtime_tick(&sims[1].rt);
    assert(nrf_cs_cycle_runtime_reply_active(&sims[1].rt));
    g_now += 1U;
    nrf_cs_cycle_runtime_tick(&sims[1].rt);
    assert(!nrf_cs_cycle_runtime_reply_active(&sims[1].rt));

    /* A node that never reported leaves its attempts missing, but the replay still completes. */
    nrf_cs_scheduler_t partial;
    assert(nrf_cs_scheduler_init(&partial, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&partial, ids, node_count) == NRF_CS_OK);
    nodes[1].count = 0U;
    const int missing = nrf_cs_cycle_replay(&partial, nodes, node_count);
    assert(missing > 0 && nrf_cs_scheduler_is_complete(&partial));
}

/* An attempt that outlives its window is cancelled and reported as a timeout; the next window is clean. */
static void test_attempt_that_never_finishes_is_aborted_at_window_end(void)
{
    static const nrf_cs_node_id_t ids[] = {0U, 1U, 2U};
    const nrf_cs_policy_t policy = make_policy();
    nrf_cs_scheduler_t planner;
    sim_node_t sims[3];

    g_now = 5000U;
    assert(nrf_cs_scheduler_init(&planner, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&planner, ids, 3U) == NRF_CS_OK);
    for (size_t n = 0U; n < 3U; ++n) init_sim(&sims[n], ids[n]);
    sims[0].latency_ms = 60000U; /* node 0 never finishes anything */
    send_plan(sims, 3U, &planner, 10U);
    run_cycle(sims, 3U, planner.cycle_id);

    assert(sims[0].aborts > 0U);
    nrf_cs_cycle_record_t records[NRF_CS_CYCLE_MAX_RECORDS];
    const size_t count = collect_records(&sims[0], records, NRF_CS_CYCLE_MAX_RECORDS);
    assert(count > 0U);
    for (size_t i = 0U; i < count; ++i) {
        assert(records[i].outcome == NRF_CS_OUTCOME_TIMEOUT);
    }
    /* Nodes 1 and 2 were not disturbed by node 0's stuck attempts. */
    assert(sims[1].aborts == 0U && sims[2].aborts == 0U);
    assert(sims[1].rt.record_count > 0U && sims[2].rt.record_count > 0U);
    for (size_t n = 0U; n < 3U; ++n) assert(sims[n].rt.state == NRF_CS_CYCLE_STATE_COMPLETE);
}

/* The BLE-idle diagnostic image refuses every attempt: the cycle still ends on time, all failures. */
static void test_refused_attempts_are_failures_not_stalls(void)
{
    static const nrf_cs_node_id_t ids[] = {0U, 1U, 2U};
    const nrf_cs_policy_t policy = make_policy();
    nrf_cs_scheduler_t planner, replay;
    sim_node_t sims[3];

    g_now = 9000U;
    assert(nrf_cs_scheduler_init(&planner, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&planner, ids, 3U) == NRF_CS_OK);
    assert(nrf_cs_scheduler_init(&replay, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&replay, ids, 3U) == NRF_CS_OK);
    for (size_t n = 0U; n < 3U; ++n) {
        init_sim(&sims[n], ids[n]);
        sims[n].refuse = -ENOTSUP;
    }
    send_plan(sims, 3U, &planner, 10U);
    run_cycle(sims, 3U, planner.cycle_id);

    static nrf_cs_cycle_record_t storage[3][NRF_CS_CYCLE_MAX_RECORDS];
    nrf_cs_cycle_node_records_t nodes[3];
    for (size_t n = 0U; n < 3U; ++n) {
        nodes[n].node = ids[n];
        nodes[n].records = storage[n];
        nodes[n].count = collect_records(&sims[n], storage[n], NRF_CS_CYCLE_MAX_RECORDS);
        for (size_t i = 0U; i < nodes[n].count; ++i) assert(storage[n][i].outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
    }
    assert(nrf_cs_cycle_replay(&replay, nodes, 3U) == 0);
    for (size_t i = 0U; i < nrf_cs_scheduler_pair_count(&replay); ++i) {
        assert(nrf_cs_scheduler_pair_at(&replay, i)->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
    }
}

/* A late attempt must not start when a whole timeout no longer fits in the window. */
static void test_no_attempt_starts_without_room_in_the_window(void)
{
    sim_node_t sim;
    nrf_cs_cycle_plan_t plan = {
        .request_id = 1U, .cycle_id = 4U, .membership_generation = 1U, .pair_timeout_ms = 500U,
        .max_attempts = 3U, .samples_per_pair = 3U, .min_confidence = 500U, .full_rotation = true,
        .step_count = 2U, .node_count = 2U, .self_node = 0U,
    };
    nrf_cs_cycle_plan_steps_t chunk = {.request_id = 2U, .cycle_id = 4U, .first_step = 0U, .step_count = 2U};
    const uint32_t window = nrf_cs_cycle_window_ms(500U, 3U, 3U);
    const nrf_cs_cycle_go_t go = {.request_id = 3U, .cycle_id = 4U, .total_timeout_ms = window * 2U + NRF_CS_CYCLE_TOTAL_MARGIN_MS};

    g_now = 100000U;
    init_sim(&sim, 0U);
    chunk.steps[0] = (nrf_cs_cycle_step_t){.initiator = 0U, .reflector = 1U};
    chunk.steps[1] = (nrf_cs_cycle_step_t){.initiator = 1U, .reflector = 0U};
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 0U, &plan) == 0);
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == 0);
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == 0);

    const uint64_t t0 = g_now;
    g_now = t0 + NRF_CS_CYCLE_GO_SKEW_MS - 1U;
    sim_tick(&sim);
    assert(sim.executes == 0U);                      /* too early: the skew margin has not passed */
    g_now = t0 + NRF_CS_CYCLE_GO_SKEW_MS;
    sim_tick(&sim);
    assert(sim.executes == 1U && sim.active);
    sim.active = false;                               /* the attempt fails right away */
    {
        const nrf_cs_measurement_t failed = {.outcome = NRF_CS_OUTCOME_TIMEOUT};
        assert(nrf_cs_cycle_runtime_step_done(&sim.rt, &sim.step, &failed) == 0);
    }
    /* Only 100 ms of the window are left, less than the 500 ms an attempt may take. */
    g_now = t0 + window - 100U;
    sim_tick(&sim);
    sim_tick(&sim);
    assert(sim.executes == 1U && sim.rt.window_exhausted && !sim.rt.attempt_active);
    /* The next window belongs to node 1, so node 0 stays idle. */
    g_now = t0 + window + NRF_CS_CYCLE_GO_SKEW_MS + 1000U;
    sim_tick(&sim);
    assert(sim.executes == 1U && sim.rt.window == 1U);
    /* After the last window the cycle completes. */
    g_now = t0 + (uint64_t)window * 2U;
    nrf_cs_cycle_runtime_tick(&sim.rt);
    assert(sim.rt.state == NRF_CS_CYCLE_STATE_COMPLETE);
    assert(sim.rt.record_count == 1U);
}

/* A busy BLE side delays an attempt without spending the retry budget. */
static void test_busy_ble_side_delays_but_does_not_fail_an_attempt(void)
{
    sim_node_t sim;
    nrf_cs_cycle_plan_t plan = {
        .request_id = 1U, .cycle_id = 4U, .membership_generation = 1U, .pair_timeout_ms = 500U,
        .max_attempts = 3U, .samples_per_pair = 1U, .min_confidence = 500U, .full_rotation = true,
        .step_count = 2U, .node_count = 2U, .self_node = 0U,
    };
    nrf_cs_cycle_plan_steps_t chunk = {.request_id = 2U, .cycle_id = 4U, .first_step = 0U, .step_count = 2U};
    const uint32_t window = nrf_cs_cycle_window_ms(500U, 1U, 3U);
    const nrf_cs_cycle_go_t go = {.request_id = 3U, .cycle_id = 4U, .total_timeout_ms = window * 2U + NRF_CS_CYCLE_TOTAL_MARGIN_MS};

    g_now = 200000U;
    init_sim(&sim, 0U);
    sim.busy_first = 4;
    sim.always_succeed = true;
    chunk.steps[0] = (nrf_cs_cycle_step_t){.initiator = 0U, .reflector = 1U};
    chunk.steps[1] = (nrf_cs_cycle_step_t){.initiator = 1U, .reflector = 0U};
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 0U, &plan) == 0);
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == 0);
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == 0);

    const uint64_t t0 = g_now;
    uint64_t first_real_start = 0U;
    while (g_now < t0 + window) {
        g_now += 10U;
        if (sim.active && g_now >= sim.due_ms) {
            sim.active = false;
            sim.last_end_ms = g_now;
            assert(nrf_cs_cycle_runtime_step_done(&sim.rt, &sim.step, &sim.outcome) == 0);
        }
        const unsigned before = sim.executes;
        sim_tick(&sim);
        if (sim.active && first_real_start == 0U) first_real_start = g_now;
        (void)before;
    }
    /* Four busy answers (each retried after 50 ms) pushed the real start later, but the attempt then
     * succeeded and the single sample ended the execution: exactly one recorded attempt. */
    assert(first_real_start >= t0 + NRF_CS_CYCLE_GO_SKEW_MS + 4U * NRF_CS_CYCLE_BUSY_RETRY_MS);
    assert(sim.executes == 5U);
    assert(sim.rt.record_count == 1U && sim.rt.records[0].attempt == 1U);
    assert(sim.rt.records[0].outcome == NRF_CS_OUTCOME_SUCCESS);
}

static void test_plan_intake_rules(void)
{
    sim_node_t sim;
    nrf_cs_cycle_plan_t plan = {
        .request_id = 1U, .cycle_id = 4U, .membership_generation = 1U, .pair_timeout_ms = 500U,
        .max_attempts = 3U, .samples_per_pair = 3U, .min_confidence = 500U, .full_rotation = true,
        .step_count = 6U, .node_count = 3U, .self_node = 1U,
    };
    nrf_cs_cycle_plan_steps_t chunk = {.request_id = 2U, .cycle_id = 4U, .first_step = 0U, .step_count = 6U};
    const nrf_cs_cycle_go_t go = {.request_id = 3U, .cycle_id = 4U, .total_timeout_ms = 120000U};

    g_now = 100U;
    init_sim(&sim, 1U);
    for (uint8_t i = 0U; i < 6U; ++i) {
        chunk.steps[i] = (nrf_cs_cycle_step_t){.initiator = (uint8_t)(i % 3U), .reflector = (uint8_t)((i + 1U) % 3U)};
    }
    /* Nothing works before a plan, and a plan for another node is refused. */
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == -1);
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == -1);
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 2U, &plan) == -1);
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 1U, &plan) == 0);
    /* GO needs the complete step list and the right cycle. */
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == -1);
    nrf_cs_cycle_plan_steps_t wrong = chunk; wrong.cycle_id = 5U;
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &wrong) == -1);
    wrong = chunk; wrong.first_step = 3U; /* 3 + 6 steps overruns the six-step plan */
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &wrong) == -1);
    chunk.step_count = 3U;
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == 0);
    assert(!nrf_cs_cycle_runtime_plan_ready(&sim.rt) && nrf_cs_cycle_runtime_go(&sim.rt, &go) == -1);
    chunk.first_step = 3U;
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == 0);
    assert(nrf_cs_cycle_runtime_plan_ready(&sim.rt));
    nrf_cs_cycle_go_t bad_go = go; bad_go.cycle_id = 9U;
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &bad_go) == -1);
    bad_go = go; bad_go.total_timeout_ms = 1000U; /* shorter than the windows need */
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &bad_go) == -1);
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == 0);
    assert(nrf_cs_cycle_runtime_running(&sim.rt));

    /* While running: no new plan, no other GO, a repeated GO is fine. */
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 1U, &plan) == -2);
    assert(nrf_cs_cycle_runtime_accept_steps(&sim.rt, &chunk) == -2);
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &go) == 0);
    nrf_cs_cycle_go_t other = go; other.request_id = 4U;
    assert(nrf_cs_cycle_runtime_go(&sim.rt, &other) == -2);

    /* Abort ends it as FAILED and keeps the reply visible until released. */
    nrf_cs_cycle_runtime_abort(&sim.rt, NRF_CS_CYCLE_FAILURE_ABORTED);
    nrf_cs_cycle_status_t status;
    nrf_cs_cycle_runtime_status(&sim.rt, 7U, &status);
    assert(status.state == NRF_CS_CYCLE_STATE_FAILED && status.failure == NRF_CS_CYCLE_FAILURE_ABORTED);
    assert(status.request_id == 7U && nrf_cs_cycle_runtime_reply_active(&sim.rt));
    assert(!nrf_cs_cycle_runtime_running(&sim.rt));
    nrf_cs_cycle_runtime_tick(&sim.rt); /* a stopped cycle ignores ticks */
    assert(nrf_cs_cycle_runtime_accept_plan(&sim.rt, 1U, &plan) == 0); /* and a new plan may follow */
}

static void test_status_progress_and_result_page_limits(void)
{
    static const nrf_cs_node_id_t ids[] = {0U, 1U, 2U};
    const nrf_cs_policy_t policy = make_policy();
    nrf_cs_scheduler_t planner;
    sim_node_t sims[3];
    nrf_cs_cycle_status_t status;
    nrf_cs_cycle_result_t page;

    g_now = 20000U;
    assert(nrf_cs_scheduler_init(&planner, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&planner, ids, 3U) == NRF_CS_OK);
    for (size_t n = 0U; n < 3U; ++n) init_sim(&sims[n], ids[n]);
    send_plan(sims, 3U, &planner, 10U);

    nrf_cs_cycle_runtime_status(&sims[0].rt, 0U, &status);
    assert(status.state == NRF_CS_CYCLE_STATE_IDLE);
    assert(nrf_cs_cycle_runtime_result_page(&sims[0].rt, 1U, 0U, &page) != 0); /* no cycle, no pages */

    const uint32_t window = nrf_cs_cycle_window_ms(500U, 3U, 3U);
    const nrf_cs_cycle_go_t go = {.request_id = 50U, .cycle_id = planner.cycle_id, .total_timeout_ms = window * 6U + NRF_CS_CYCLE_TOTAL_MARGIN_MS};
    assert(nrf_cs_cycle_runtime_go(&sims[1].rt, &go) == 0);
    nrf_cs_cycle_runtime_status(&sims[1].rt, 0U, &status);
    assert(status.state == NRF_CS_CYCLE_STATE_RUNNING && status.request_id == 50U && status.pairs_done == 0U);

    g_now += (uint64_t)window * 2U + 10U; /* two windows have passed */
    nrf_cs_cycle_runtime_tick(&sims[1].rt);
    nrf_cs_cycle_runtime_status(&sims[1].rt, 0U, &status);
    assert(status.pairs_done == 2U && status.pairs_total == 6U && status.elapsed_ms >= window * 2U);
    assert(nrf_cs_cycle_runtime_result_page(&sims[1].rt, 0U, 0U, &page) != 0); /* a zero request id is invalid */
    assert(nrf_cs_cycle_runtime_result_page(&sims[1].rt, 1U, 9U, &page) != 0);  /* page beyond the records */
}

/* A callback can run before the next tick has closed its expired window. */
static void test_late_success_cannot_escape_window_deadline(void)
{
    const nrf_cs_node_id_t ids[] = {0U, 1U, 2U};
    const nrf_cs_policy_t policy = make_policy();
    nrf_cs_scheduler_t planner;
    sim_node_t sims[3];
    g_now = 1000U;
    assert(nrf_cs_scheduler_init(&planner, &policy) == NRF_CS_OK);
    assert(nrf_cs_scheduler_start_cycle(&planner, ids, 3U) == NRF_CS_OK);
    for (size_t n = 0; n < 3; ++n) init_sim(&sims[n], ids[n]);
    send_plan(sims, 3U, &planner, 10U);
    const nrf_cs_cycle_go_t go = {.request_id=50U, .cycle_id=planner.cycle_id,
        .total_timeout_ms=120000U};
    assert(nrf_cs_cycle_runtime_go(&sims[0].rt, &go) == 0);
    g_now += NRF_CS_CYCLE_GO_SKEW_MS;
    sim_tick(&sims[0]);
    assert(sims[0].active);
    const nrf_cs_measurement_t success = {.outcome=NRF_CS_OUTCOME_SUCCESS,
        .distance_mm=1000, .confidence=900};
    g_now = sims[0].rt.t0_ms + sims[0].rt.window_ms;
    assert(nrf_cs_cycle_runtime_step_done(&sims[0].rt, &sims[0].step, &success) == 0);
    assert(sims[0].rt.record_count == 1);
    assert(sims[0].rt.records[0].outcome == NRF_CS_OUTCOME_TIMEOUT);
    assert(sims[0].rt.records[0].confidence == 0);
    assert(nrf_cs_cycle_runtime_step_done(&sims[0].rt, &sims[0].step, &success) == -1);
}

int main(void)
{
    test_late_success_cannot_escape_window_deadline();
    test_autonomous_cycle_matches_live_scheduler(3U);
    test_autonomous_cycle_matches_live_scheduler(4U);
    test_autonomous_cycle_matches_live_scheduler(5U);
    test_attempt_that_never_finishes_is_aborted_at_window_end();
    test_refused_attempts_are_failures_not_stalls();
    test_no_attempt_starts_without_room_in_the_window();
    test_busy_ble_side_delays_but_does_not_fail_an_attempt();
    test_plan_intake_rules();
    test_status_progress_and_result_page_limits();
    puts("nRF CS cycle runtime: PASS");
    return 0;
}
