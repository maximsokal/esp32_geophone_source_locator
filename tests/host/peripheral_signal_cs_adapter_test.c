/*
 * Channel Sounding pair procedure (connect -> secure -> MTU -> RAS discovery ->
 * features -> capabilities -> config -> CS security -> procedure -> cleanup).
 * Exhaustive phase x event matrix plus failure injection for every operation.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_pair_adapter.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)

typedef enum {
    OP_CONNECT, OP_SECURE, OP_MTU, OP_DISCOVER, OP_FEATURES, OP_RANGING_DATA,
    OP_CAPABILITIES, OP_CONFIG, OP_CS_SECURITY, OP_PROCEDURE, OP_CLEANUP, OP_COUNT
} op_t;

typedef struct {
    int fail_op; /* -1: none */
    unsigned calls[OP_COUNT];
    unsigned order[64];
    unsigned order_count;
    bool realtime_seen;
    nrf_cs_node_id_t reflector_seen;
    unsigned completions;
    nrf_cs_step_t completed_step;
    nrf_cs_measurement_t completed_measurement;
} fixture_t;

static int hit(fixture_t *f, op_t op)
{
    ++f->calls[op];
    if (f->order_count < 64U) f->order[f->order_count++] = (unsigned)op;
    return f->fail_op == (int)op ? -5 : 0;
}

static int op_connect(void *c, nrf_cs_node_id_t reflector)
{
    fixture_t *f = c;
    f->reflector_seen = reflector;
    return hit(f, OP_CONNECT);
}
static int op_secure(void *c) { return hit(c, OP_SECURE); }
static int op_mtu(void *c) { return hit(c, OP_MTU); }
static int op_discover(void *c) { return hit(c, OP_DISCOVER); }
static int op_features(void *c) { return hit(c, OP_FEATURES); }
static int op_ranging_data(void *c, bool realtime)
{
    fixture_t *f = c;
    f->realtime_seen = realtime;
    return hit(f, OP_RANGING_DATA);
}
static int op_capabilities(void *c) { return hit(c, OP_CAPABILITIES); }
static int op_config(void *c) { return hit(c, OP_CONFIG); }
static int op_cs_security(void *c) { return hit(c, OP_CS_SECURITY); }
static int op_procedure(void *c) { return hit(c, OP_PROCEDURE); }
static int op_cleanup(void *c) { return hit(c, OP_CLEANUP); }

static void on_complete(const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement, void *c)
{
    fixture_t *f = c;
    ++f->completions;
    f->completed_step = *step;
    f->completed_measurement = *measurement;
}

static nrf_cs_pair_adapter_ops_t make_ops(fixture_t *f)
{
    nrf_cs_pair_adapter_ops_t ops = {
        .connect = op_connect, .secure = op_secure, .exchange_mtu = op_mtu,
        .discover_ras = op_discover, .read_features = op_features,
        .configure_ranging_data = op_ranging_data,
        .exchange_capabilities = op_capabilities, .create_config = op_config,
        .enable_cs_security = op_cs_security, .start_procedure = op_procedure,
        .cleanup = op_cleanup, .context = f,
    };
    return ops;
}

static nrf_cs_step_t make_step(void)
{
    nrf_cs_step_t step;
    memset(&step, 0, sizeof(step));
    step.cycle_id = 4U;
    step.membership_generation = 2U;
    step.attempt_id = 9U;
    step.pair_index = 1U;
    step.attempt_number = 1U;
    step.initiator = 0U;
    step.reflector = 2U;
    step.timeout_ms = 500U;
    return step;
}

static bool same_step(const nrf_cs_step_t *a, const nrf_cs_step_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

typedef nrf_cs_pair_adapter_result_t (*simple_event_t)(nrf_cs_pair_adapter_t *, const nrf_cs_step_t *);

static nrf_cs_pair_adapter_result_t ev_connected(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_connected(a, s); }
static nrf_cs_pair_adapter_result_t ev_security(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_security_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_mtu(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_mtu_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_discovery(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_discovery_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_features(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_features_ready(a, s, true); }
static nrf_cs_pair_adapter_result_t ev_capabilities(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_capabilities_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_config(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_config_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_cs_security(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_cs_security_ready(a, s); }
static nrf_cs_pair_adapter_result_t ev_measurement(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s)
{
    nrf_cs_measurement_t m;
    memset(&m, 0, sizeof(m));
    m.outcome = NRF_CS_OUTCOME_SUCCESS;
    m.distance_mm = 1500;
    m.confidence = 700U;
    m.provider_quality_flags = 0x0006U;
    return nrf_cs_pair_adapter_measurement_ready(a, s, &m);
}
static nrf_cs_pair_adapter_result_t ev_transport(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_transport_error(a, s); }
static nrf_cs_pair_adapter_result_t ev_timeout(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_timeout(a, s); }
static nrf_cs_pair_adapter_result_t ev_cleanup(nrf_cs_pair_adapter_t *a, const nrf_cs_step_t *s) { return nrf_cs_pair_adapter_cleanup_complete(a, s); }

enum { EVENT_COUNT = 12, CHAIN_EVENTS = 9 };

static const struct {
    simple_event_t fn;
    /* Phase in which the event is the normal next step; -1 for terminal events. */
    int valid_phase;
    int next_phase;
} k_events[EVENT_COUNT] = {
    {ev_connected, NRF_CS_PAIR_PHASE_CONNECTING, NRF_CS_PAIR_PHASE_SECURING},
    {ev_security, NRF_CS_PAIR_PHASE_SECURING, NRF_CS_PAIR_PHASE_MTU},
    {ev_mtu, NRF_CS_PAIR_PHASE_MTU, NRF_CS_PAIR_PHASE_DISCOVERING},
    {ev_discovery, NRF_CS_PAIR_PHASE_DISCOVERING, NRF_CS_PAIR_PHASE_FEATURES},
    {ev_features, NRF_CS_PAIR_PHASE_FEATURES, NRF_CS_PAIR_PHASE_CAPABILITIES},
    {ev_capabilities, NRF_CS_PAIR_PHASE_CAPABILITIES, NRF_CS_PAIR_PHASE_CONFIG},
    {ev_config, NRF_CS_PAIR_PHASE_CONFIG, NRF_CS_PAIR_PHASE_CS_SECURITY},
    {ev_cs_security, NRF_CS_PAIR_PHASE_CS_SECURITY, NRF_CS_PAIR_PHASE_PROCEDURE},
    {ev_measurement, NRF_CS_PAIR_PHASE_PROCEDURE, NRF_CS_PAIR_PHASE_CLEANUP},
    {ev_transport, -1, NRF_CS_PAIR_PHASE_CLEANUP},
    {ev_timeout, -1, NRF_CS_PAIR_PHASE_CLEANUP},
    {ev_cleanup, NRF_CS_PAIR_PHASE_CLEANUP, NRF_CS_PAIR_PHASE_IDLE},
};

static void start(nrf_cs_pair_adapter_t *adapter, fixture_t *fixture, const nrf_cs_step_t *step, int fail_op)
{
    nrf_cs_pair_adapter_ops_t ops;
    memset(fixture, 0, sizeof(*fixture));
    fixture->fail_op = fail_op;
    ops = make_ops(fixture);
    assert(nrf_cs_pair_adapter_init(adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_execute(adapter, step->initiator, step, on_complete, fixture) ==
           NRF_CS_PAIR_ADAPTER_OK);
}

/* Drive a fresh adapter forward until it sits in `target` (no failures injected). */
static void drive_to(nrf_cs_pair_adapter_t *adapter, fixture_t *fixture, const nrf_cs_step_t *step,
    nrf_cs_pair_phase_t target)
{
    start(adapter, fixture, step, -1);
    for (int index = 0; index < CHAIN_EVENTS; ++index) {
        if (nrf_cs_pair_adapter_phase(adapter) == target) return;
        assert(k_events[index].fn(adapter, step) == NRF_CS_PAIR_ADAPTER_OK);
    }
    assert(nrf_cs_pair_adapter_phase(adapter) == target);
}

static nrf_cs_pair_adapter_result_t expected_result(nrf_cs_pair_phase_t phase, int event)
{
    if (k_events[event].valid_phase >= 0) {
        if ((int)phase == k_events[event].valid_phase) {
            return event == 11 ? NRF_CS_PAIR_ADAPTER_COMPLETED : NRF_CS_PAIR_ADAPTER_OK;
        }
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }
    /* transport_error / timeout: accepted in every working phase. */
    return phase == NRF_CS_PAIR_PHASE_CLEANUP ? NRF_CS_PAIR_ADAPTER_ERR_STATE : NRF_CS_PAIR_ADAPTER_OK;
}

static void every_phase_and_event_follows_the_transition_table(void)
{
    unsigned checked = 0U;

    for (int phase = NRF_CS_PAIR_PHASE_CONNECTING; phase <= NRF_CS_PAIR_PHASE_CLEANUP; ++phase) {
        for (int event = 0; event < EVENT_COUNT; ++event) {
            nrf_cs_pair_adapter_t adapter;
            fixture_t fixture;
            const nrf_cs_step_t step = make_step();

            /* CLEANUP is reached through a terminal event. */
            if (phase == NRF_CS_PAIR_PHASE_CLEANUP) {
                drive_to(&adapter, &fixture, &step, NRF_CS_PAIR_PHASE_PROCEDURE);
                assert(ev_measurement(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
            } else {
                drive_to(&adapter, &fixture, &step, (nrf_cs_pair_phase_t)phase);
            }
            assert(nrf_cs_pair_adapter_phase(&adapter) == (nrf_cs_pair_phase_t)phase);

            const nrf_cs_pair_adapter_result_t expected = expected_result((nrf_cs_pair_phase_t)phase, event);
            const nrf_cs_pair_adapter_result_t actual = k_events[event].fn(&adapter, &step);
            assert(actual == expected);
            ++checked;

            if (expected == NRF_CS_PAIR_ADAPTER_OK || expected == NRF_CS_PAIR_ADAPTER_COMPLETED) {
                assert(nrf_cs_pair_adapter_phase(&adapter) == (nrf_cs_pair_phase_t)k_events[event].next_phase);
            } else {
                /* A rejected event changes nothing and does not complete. */
                assert(nrf_cs_pair_adapter_phase(&adapter) == (nrf_cs_pair_phase_t)phase);
                assert(nrf_cs_pair_adapter_is_active(&adapter));
            }
            assert(fixture.completions == (expected == NRF_CS_PAIR_ADAPTER_COMPLETED ? 1U : 0U));
        }
    }
    assert(checked == 10U * EVENT_COUNT);
    CASE_PASS("CS.ADAPTER.phase_event_matrix");
}

static void stale_or_foreign_steps_never_move_the_machine(void)
{
    nrf_cs_pair_adapter_t adapter;
    fixture_t fixture;
    const nrf_cs_step_t step = make_step();

    for (int phase = NRF_CS_PAIR_PHASE_CONNECTING; phase <= NRF_CS_PAIR_PHASE_PROCEDURE; ++phase) {
        for (int field = 0; field < 7; ++field) {
            for (int event = 0; event < EVENT_COUNT; ++event) {
                nrf_cs_step_t other = step;
                drive_to(&adapter, &fixture, &step, (nrf_cs_pair_phase_t)phase);
                switch (field) {
                case 0: other.cycle_id += 1U; break;
                case 1: other.membership_generation += 1U; break;
                case 2: other.attempt_id += 1U; break;
                case 3: other.pair_index += 1U; break;
                case 4: other.attempt_number += 1U; break;
                case 5: other.initiator += 1U; break;
                default: other.reflector += 1U; break;
                }
                const unsigned calls_before = fixture.order_count;
                assert(k_events[event].fn(&adapter, &other) == NRF_CS_PAIR_ADAPTER_ERR_STALE);
                assert(nrf_cs_pair_adapter_phase(&adapter) == (nrf_cs_pair_phase_t)phase);
                assert(fixture.order_count == calls_before);
                assert(fixture.completions == 0U);
            }
        }
    }
    /* timeout_ms is not part of the identity: a changed value still matches. */
    nrf_cs_step_t other = step;
    drive_to(&adapter, &fixture, &step, NRF_CS_PAIR_PHASE_CONNECTING);
    other.timeout_ms += 1U;
    assert(nrf_cs_pair_adapter_connected(&adapter, &other) == NRF_CS_PAIR_ADAPTER_OK);
    CASE_PASS("CS.ADAPTER.stale_step_identity");
}

static void an_idle_adapter_rejects_every_event(void)
{
    for (int event = 0; event < EVENT_COUNT; ++event) {
        nrf_cs_pair_adapter_t adapter;
        fixture_t fixture;
        nrf_cs_pair_adapter_ops_t ops;
        const nrf_cs_step_t step = make_step();
        memset(&fixture, 0, sizeof(fixture));
        fixture.fail_op = -1;
        ops = make_ops(&fixture);
        assert(nrf_cs_pair_adapter_init(&adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
        assert(!nrf_cs_pair_adapter_is_active(&adapter));
        assert(nrf_cs_pair_adapter_active_step(&adapter) == NULL);
        assert(k_events[event].fn(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STALE);
        assert(k_events[event].fn(NULL, &step) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
        assert(k_events[event].fn(&adapter, NULL) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
        assert(fixture.order_count == 0U);
    }
    assert(nrf_cs_pair_adapter_phase(NULL) == NRF_CS_PAIR_PHASE_IDLE);
    assert(!nrf_cs_pair_adapter_is_active(NULL));
    assert(nrf_cs_pair_adapter_active_step(NULL) == NULL);
    CASE_PASS("CS.ADAPTER.idle_and_null_events");
}

static void init_and_execute_argument_domain(void)
{
    nrf_cs_pair_adapter_t adapter;
    fixture_t fixture;
    nrf_cs_pair_adapter_ops_t ops;
    nrf_cs_step_t step = make_step();

    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_op = -1;
    ops = make_ops(&fixture);

    /* Every operation is mandatory. */
    assert(nrf_cs_pair_adapter_init(NULL, &ops) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    assert(nrf_cs_pair_adapter_init(&adapter, NULL) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    for (int missing = 0; missing < 11; ++missing) {
        nrf_cs_pair_adapter_ops_t broken = ops;
        switch (missing) {
        case 0: broken.connect = NULL; break;
        case 1: broken.secure = NULL; break;
        case 2: broken.exchange_mtu = NULL; break;
        case 3: broken.discover_ras = NULL; break;
        case 4: broken.read_features = NULL; break;
        case 5: broken.configure_ranging_data = NULL; break;
        case 6: broken.exchange_capabilities = NULL; break;
        case 7: broken.create_config = NULL; break;
        case 8: broken.enable_cs_security = NULL; break;
        case 9: broken.start_procedure = NULL; break;
        default: broken.cleanup = NULL; break;
        }
        assert(nrf_cs_pair_adapter_init(&adapter, &broken) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    }

    assert(nrf_cs_pair_adapter_init(&adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_execute(NULL, 0U, &step, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    assert(nrf_cs_pair_adapter_execute(&adapter, 0U, NULL, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    assert(nrf_cs_pair_adapter_execute(&adapter, 0U, &step, NULL, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    step.timeout_ms = 0U;
    assert(nrf_cs_pair_adapter_execute(&adapter, 0U, &step, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    step = make_step();
    step.reflector = step.initiator;
    assert(nrf_cs_pair_adapter_execute(&adapter, 0U, &step, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    step = make_step();
    /* Only the initiator may run a step. */
    assert(nrf_cs_pair_adapter_execute(&adapter, step.reflector, &step, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
    assert(!nrf_cs_pair_adapter_is_active(&adapter));
    assert(fixture.order_count == 0U);

    assert(nrf_cs_pair_adapter_execute(&adapter, step.initiator, &step, on_complete, &fixture) == NRF_CS_PAIR_ADAPTER_OK);
    assert(fixture.reflector_seen == step.reflector);
    {
        nrf_cs_step_t second = step;
        second.attempt_id += 1U;
        assert(nrf_cs_pair_adapter_execute(&adapter, second.initiator, &second, on_complete, &fixture) ==
               NRF_CS_PAIR_ADAPTER_ERR_BUSY);
    }
    /* The rejected second request leaves the running one untouched. */
    assert(same_step(nrf_cs_pair_adapter_active_step(&adapter), &step));
    assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CONNECTING);
    assert(fixture.calls[OP_CONNECT] == 1U);
    CASE_PASS("CS.ADAPTER.argument_and_busy_domain");
}

static void the_happy_path_calls_every_operation_once_in_order(void)
{
    static const unsigned expected_order[] = {
        OP_CONNECT, OP_SECURE, OP_MTU, OP_DISCOVER, OP_FEATURES, OP_RANGING_DATA,
        OP_CAPABILITIES, OP_CONFIG, OP_CS_SECURITY, OP_PROCEDURE, OP_CLEANUP,
    };

    for (int realtime = 0; realtime <= 1; ++realtime) {
        nrf_cs_pair_adapter_t adapter;
        fixture_t fixture;
        const nrf_cs_step_t step = make_step();

        start(&adapter, &fixture, &step, -1);
        for (int index = 0; index < CHAIN_EVENTS; ++index) {
            if (index == 4) {
                assert(nrf_cs_pair_adapter_features_ready(&adapter, &step, realtime != 0) == NRF_CS_PAIR_ADAPTER_OK);
            } else {
                assert(k_events[index].fn(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
            }
        }
        assert(fixture.realtime_seen == (realtime != 0));
        assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);

        assert(fixture.order_count == sizeof(expected_order) / sizeof(expected_order[0]));
        for (size_t i = 0U; i < fixture.order_count; ++i) assert(fixture.order[i] == expected_order[i]);
        for (int op = 0; op < OP_COUNT; ++op) assert(fixture.calls[op] == 1U);

        /* The measurement reaches the scheduler unchanged, exactly once. */
        assert(fixture.completions == 1U);
        assert(same_step(&fixture.completed_step, &step));
        assert(fixture.completed_measurement.outcome == NRF_CS_OUTCOME_SUCCESS);
        assert(fixture.completed_measurement.distance_mm == 1500);
        assert(fixture.completed_measurement.confidence == 700U);
        assert(fixture.completed_measurement.provider_quality_flags == 0x0006U);

        /* Finished: idle again and reusable for the next attempt. */
        assert(!nrf_cs_pair_adapter_is_active(&adapter));
        assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_IDLE);
        assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STALE);
        {
            nrf_cs_step_t next = step;
            next.attempt_id += 1U;
            assert(nrf_cs_pair_adapter_execute(&adapter, next.initiator, &next, on_complete, &fixture) ==
                   NRF_CS_PAIR_ADAPTER_OK);
            assert(same_step(nrf_cs_pair_adapter_active_step(&adapter), &next));
        }
    }
    CASE_PASS("CS.ADAPTER.happy_path_order");
}

static void any_operation_failure_ends_in_transport_error_without_a_distance(void)
{
    for (int failing = 0; failing < OP_COUNT; ++failing) {
        nrf_cs_pair_adapter_t adapter;
        fixture_t fixture;
        const nrf_cs_step_t step = make_step();
        bool reached_cleanup = false;

        start(&adapter, &fixture, &step, failing);
        for (int index = 0; index < CHAIN_EVENTS && !reached_cleanup; ++index) {
            if (nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP) {
                reached_cleanup = true;
                break;
            }
            const nrf_cs_pair_adapter_result_t result = k_events[index].fn(&adapter, &step);
            assert(result == NRF_CS_PAIR_ADAPTER_OK);
        }
        assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_CLEANUP);

        /* Once failed, further procedure events are refused. */
        assert(ev_security(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STATE);
        assert(ev_measurement(&adapter, &step) == NRF_CS_PAIR_ADAPTER_ERR_STATE);

        assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
        assert(fixture.completions == 1U);
        assert(fixture.completed_measurement.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
        assert(fixture.completed_measurement.distance_mm == 0);
        assert(fixture.completed_measurement.confidence == 0U);
        assert(fixture.calls[OP_CLEANUP] == 1U);
        assert(fixture.calls[failing] == 1U);
        /* No later operation runs after the failing one. */
        for (int op = failing + 1; op < OP_CLEANUP; ++op) assert(fixture.calls[op] == 0U);
    }
    CASE_PASS("CS.ADAPTER.operation_failures");
}

static void a_failed_cleanup_overrides_any_terminal_outcome(void)
{
    for (int kind = 0; kind < 3; ++kind) {
        nrf_cs_pair_adapter_t adapter;
        fixture_t fixture;
        const nrf_cs_step_t step = make_step();

        drive_to(&adapter, &fixture, &step, NRF_CS_PAIR_PHASE_PROCEDURE);
        fixture.fail_op = OP_CLEANUP;
        if (kind == 0) assert(ev_measurement(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
        if (kind == 1) assert(ev_timeout(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
        if (kind == 2) assert(ev_transport(&adapter, &step) == NRF_CS_PAIR_ADAPTER_OK);
        assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
        /* A link that could not be released is not a trustworthy measurement. */
        assert(fixture.completed_measurement.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
        assert(fixture.completed_measurement.distance_mm == 0);
        assert(fixture.completed_measurement.confidence == 0U);
    }
    CASE_PASS("CS.ADAPTER.cleanup_failure");
}

static void timeout_and_transport_events_map_to_their_outcomes(void)
{
    for (int phase = NRF_CS_PAIR_PHASE_CONNECTING; phase <= NRF_CS_PAIR_PHASE_PROCEDURE; ++phase) {
        for (int kind = 0; kind < 2; ++kind) {
            nrf_cs_pair_adapter_t adapter;
            fixture_t fixture;
            const nrf_cs_step_t step = make_step();
            drive_to(&adapter, &fixture, &step, (nrf_cs_pair_phase_t)phase);
            assert((kind == 0 ? ev_timeout(&adapter, &step) : ev_transport(&adapter, &step)) ==
                   NRF_CS_PAIR_ADAPTER_OK);
            assert(nrf_cs_pair_adapter_cleanup_complete(&adapter, &step) == NRF_CS_PAIR_ADAPTER_COMPLETED);
            assert(fixture.completed_measurement.outcome ==
                   (kind == 0 ? NRF_CS_OUTCOME_TIMEOUT : NRF_CS_OUTCOME_TRANSPORT_ERROR));
            assert(fixture.completed_measurement.distance_mm == 0);
            assert(fixture.completed_measurement.confidence == 0U);
            assert(fixture.calls[OP_CLEANUP] == 1U);
        }
    }
    {
        nrf_cs_pair_adapter_t adapter;
        fixture_t fixture;
        const nrf_cs_step_t step = make_step();
        drive_to(&adapter, &fixture, &step, NRF_CS_PAIR_PHASE_PROCEDURE);
        assert(nrf_cs_pair_adapter_measurement_ready(&adapter, &step, NULL) == NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT);
        assert(nrf_cs_pair_adapter_phase(&adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);
    }
    CASE_PASS("CS.ADAPTER.terminal_outcome_mapping");
}

int main(void)
{
    /* Progress lines must survive a crash in a later case. */
    setvbuf(stdout, NULL, _IONBF, 0);
    every_phase_and_event_follows_the_transition_table();
    stale_or_foreign_steps_never_move_the_machine();
    an_idle_adapter_rejects_every_event();
    init_and_execute_argument_domain();
    the_happy_path_calls_every_operation_once_in_order();
    any_operation_failure_ends_in_transport_error_without_a_distance();
    a_failed_cleanup_overrides_any_terminal_outcome();
    timeout_and_transport_events_map_to_their_outcomes();
    return 0;
}
