#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_authority.h"

typedef struct {
    uint64_t now_ms;
    nrf_cs_control_request_t last_request;
    bool have_request;
    uint32_t last_unique_attempt;
    unsigned unique_initiator_turns[NRF_CS_MAX_NODES + 1U];
    unsigned write_calls;
    unsigned poll_calls;
    unsigned fail_first_write;
    unsigned busy_polls;
    unsigned failed_polls;
    unsigned stale_polls;
} fake_transport_t;

static int fake_write_execute(void *context, const nrf_cs_control_request_t *request)
{
    fake_transport_t *transport = context;
    ++transport->write_calls;
    transport->last_request = *request;
    transport->have_request = true;
    if (request->step.attempt_id != transport->last_unique_attempt) {
        transport->last_unique_attempt = request->step.attempt_id;
        if (request->step.initiator <= NRF_CS_MAX_NODES) {
            ++transport->unique_initiator_turns[request->step.initiator];
        }
    }
    if (transport->fail_first_write > 0U) {
        --transport->fail_first_write;
        return -1;
    }
    return 0;
}

static int fake_poll_result(void *context, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *result)
{
    fake_transport_t *transport = context;
    ++transport->poll_calls;
    assert(transport->have_request);
    assert(request->request_id == transport->last_request.request_id);
    assert(nrf_cs_control_step_equal(&request->step, &transport->last_request.step));
    if (transport->failed_polls > 0U) { --transport->failed_polls; return -1; }
    memset(result, 0, sizeof(*result));
    result->request_id = request->request_id;
    result->step = request->step;
    if (transport->stale_polls > 0U) { --transport->stale_polls; --result->request_id; }
    if (transport->busy_polls > 0U) {
        --transport->busy_polls;
        result->state = NRF_CS_CONTROL_STATE_FAILED;
        result->command_result = NRF_CS_CONTROL_RESULT_BUSY;
        result->measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
        return 0;
    }
    result->state = NRF_CS_CONTROL_STATE_COMPLETE;
    result->command_result = NRF_CS_CONTROL_RESULT_OK;
    result->measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    result->measurement.distance_mm = 1000 + (int32_t)request->step.pair_index;
    result->measurement.confidence = 1000U;
    return 0;
}

static uint64_t fake_now_ms(void *context)
{
    return ((fake_transport_t *)context)->now_ms;
}

static void run_to_completion(nrf_cs_authority_t *authority, fake_transport_t *transport)
{
    unsigned guard = 0U;
    while (nrf_cs_authority_is_active(authority)) {
        nrf_cs_authority_result_t result = nrf_cs_authority_pump(authority);
        assert(result == NRF_CS_AUTHORITY_PROGRESS || result == NRF_CS_AUTHORITY_COMPLETE);
        ++transport->now_ms;
        assert(++guard < 200U);
    }
}

int main(void)
{
    const nrf_cs_policy_t policy = {.pair_timeout_ms = 100U, .max_attempts = 2U, .min_confidence = 500U};
    const nrf_cs_node_id_t five_nodes[] = {5U, 1U, 3U, 2U, 4U};
    fake_transport_t transport;
    nrf_cs_authority_ops_t ops;
    nrf_cs_authority_t authority;
    size_t i;

    memset(&transport, 0, sizeof(transport));
    memset(&ops, 0, sizeof(ops));
    ops.context = &transport;
    ops.write_execute = fake_write_execute;
    ops.poll_result = fake_poll_result;
    ops.now_ms = fake_now_ms;

    {
        const nrf_cs_node_id_t nodes[] = {1U, 2U};
        const nrf_cs_policy_t single = {.pair_timeout_ms = 100U, .max_attempts = 1U, .min_confidence = 500U};
        assert(nrf_cs_authority_init(&authority, &single, &ops, 1U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_start_cycle(&authority, nodes, 2U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        const uint32_t request = authority.request.request_id;
        const uint32_t attempt = authority.request.step.attempt_id;
        transport.failed_polls = 1U;
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(authority.command_active && authority.execute_acked);
        assert(authority.request.request_id == request && authority.request.step.attempt_id == attempt);
        transport.stale_polls = 1U;
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_ERR_STALE);
        assert(authority.command_active && authority.request.request_id == request);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(!authority.command_active && transport.write_calls == 1U);
        run_to_completion(&authority, &transport);
        assert(transport.write_calls == 2U); /* two-node mode has two independent executions */
        assert(nrf_cs_authority_pair_at(&authority, 0U)->state == NRF_CS_PAIR_VALID);

        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_init(&authority, &single, &ops, 2U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_start_cycle(&authority, nodes, 2U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        transport.failed_polls = 1000U;
        for (transport.now_ms = 1U; transport.now_ms < 100U; ++transport.now_ms) {
            assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
            assert(authority.command_active);
        }
        const unsigned polls = transport.poll_calls;
        (void)nrf_cs_authority_pump(&authority); /* exact deadline: no more I2C reads */
        assert(transport.poll_calls == polls && !authority.command_active);
        run_to_completion(&authority, &transport);
        assert(transport.write_calls == 2U);
        assert(nrf_cs_authority_pair_at(&authority, 0U)->last_failure == NRF_CS_FAILURE_TIMEOUT);
        memset(&transport, 0, sizeof(transport));
    }

    assert(nrf_cs_authority_init(&authority, &policy, &ops, 100U) == NRF_CS_AUTHORITY_OK);
    transport.fail_first_write = 1U;
    transport.busy_polls = 1U;
    assert(nrf_cs_authority_start_cycle(&authority, five_nodes, 5U) == NRF_CS_AUTHORITY_PROGRESS);
    run_to_completion(&authority, &transport);
    assert(nrf_cs_authority_pair_count(&authority) == 10U);
    assert(transport.write_calls == 12U);
    assert(transport.poll_calls == 11U);
    for (i = 1U; i <= 5U; ++i) {
        assert(transport.unique_initiator_turns[i] == 2U);
    }
    for (i = 0U; i < 10U; ++i) {
        const nrf_cs_pair_result_t *pair = nrf_cs_authority_pair_at(&authority, i);
        assert(pair != NULL);
        assert(pair->state == NRF_CS_PAIR_VALID);
        assert(pair->confidence == 1000U);
    }

    {
        const nrf_cs_policy_t rotation_policy = {
            .pair_timeout_ms = 100U,
            .max_attempts = 1U,
            .min_confidence = 500U,
            .full_rotation = true,
        };
        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_init(&authority, &rotation_policy, &ops, 125U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_start_cycle(&authority, five_nodes, 5U) == NRF_CS_AUTHORITY_PROGRESS);
        run_to_completion(&authority, &transport);
        assert(transport.write_calls == 20U);
        assert(transport.poll_calls == 20U);
        assert(nrf_cs_scheduler_execution_count(&authority.scheduler) == 20U);
        for (i = 1U; i <= 5U; ++i) {
            assert(transport.unique_initiator_turns[i] == 4U);
        }
        for (i = 0U; i < 10U; ++i) {
            const nrf_cs_pair_result_t *pair = nrf_cs_authority_pair_at(&authority, i);
            assert(pair != NULL && pair->state == NRF_CS_PAIR_VALID);
            assert(!pair->single_direction);
            assert(pair->direction_disagreement_mm == 0);
        }
    }

    {
        const nrf_cs_policy_t timeout_policy = {
            .pair_timeout_ms = 100U,
            .max_attempts = 1U,
            .min_confidence = 500U,
            .full_rotation = true,
        };
        const nrf_cs_node_id_t three_nodes[] = {1U, 2U, 3U};
        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_init(&authority, &timeout_policy, &ops, 175U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_abort_cycle(NULL, NRF_CS_FAILURE_TIMEOUT) == NRF_CS_AUTHORITY_ERR_ARGUMENT);
        assert(nrf_cs_authority_abort_cycle(&authority, NRF_CS_FAILURE_NONE) == NRF_CS_AUTHORITY_ERR_ARGUMENT);
        assert(nrf_cs_authority_start_cycle(&authority, three_nodes, 3U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(authority.command_active);
        assert(nrf_cs_authority_abort_cycle(&authority, NRF_CS_FAILURE_TIMEOUT) == NRF_CS_AUTHORITY_COMPLETE);
        assert(!nrf_cs_authority_is_active(&authority));
        assert(!authority.command_active);
        assert(nrf_cs_scheduler_executions_done(&authority.scheduler) == 6U);
        for (i = 0U; i < 3U; ++i) {
            const nrf_cs_pair_result_t *pair = nrf_cs_authority_pair_at(&authority, i);
            assert(pair != NULL && pair->state == NRF_CS_PAIR_RETRY_EXHAUSTED);
            assert(pair->last_failure == NRF_CS_FAILURE_TIMEOUT);
        }
    }

    {
        const nrf_cs_node_id_t two_nodes[] = {1U, 2U};
        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_init(&authority, &policy, &ops, 150U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_start_cycle(&authority, two_nodes, 2U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(authority.execute_acked);
        transport.now_ms = authority.deadline_ms;
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(!authority.command_active);
        assert(authority.scheduler.pairs[0].scheduler_quality_flags & NRF_CS_QUALITY_TIMEOUT);
        run_to_completion(&authority, &transport);
        assert(nrf_cs_authority_pair_at(&authority, 0U)->state == NRF_CS_PAIR_VALID);
    }

    {
        const nrf_cs_node_id_t three_nodes[] = {1U, 2U, 3U};
        uint32_t first_generation;
        uint32_t next_generation;
        nrf_cs_step_t active_step;

        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_init(&authority, &policy, &ops, 200U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_start_cycle(&authority, three_nodes, 3U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(authority.command_active);
        active_step = authority.request.step;
        first_generation = active_step.membership_generation;
        assert(nrf_cs_authority_disconnect(&authority, active_step.reflector) == NRF_CS_AUTHORITY_PROGRESS);
        assert(!authority.command_active);
        run_to_completion(&authority, &transport);
        assert(nrf_cs_authority_start_cycle(&authority, three_nodes, 3U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        next_generation = authority.request.step.membership_generation;
        assert(next_generation != first_generation);
    }

    {
        /* Autonomous ranging completes the scheduler from outside; the authority only has to end the cycle. */
        const nrf_cs_node_id_t three_nodes[] = {1U, 2U, 3U};
        nrf_cs_step_t step;
        const nrf_cs_measurement_t good = {.outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1500, .confidence = 900U};

        memset(&transport, 0, sizeof(transport));
        assert(nrf_cs_authority_finish_external(NULL) == NRF_CS_AUTHORITY_ERR_ARGUMENT);
        assert(nrf_cs_authority_init(&authority, &policy, &ops, 300U) == NRF_CS_AUTHORITY_OK);
        assert(nrf_cs_authority_finish_external(&authority) == NRF_CS_AUTHORITY_ERR_STATE); /* no cycle */
        assert(nrf_cs_authority_start_cycle(&authority, three_nodes, 3U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_finish_external(&authority) == NRF_CS_AUTHORITY_ERR_STATE); /* not complete yet */
        assert(nrf_cs_authority_is_active(&authority));
        while (nrf_cs_scheduler_next_step(&authority.scheduler, &step) == NRF_CS_STEP_READY) {
            assert(nrf_cs_scheduler_complete_step(&authority.scheduler, &step, &good) == NRF_CS_OK);
        }
        assert(nrf_cs_scheduler_is_complete(&authority.scheduler));
        assert(nrf_cs_authority_is_active(&authority)); /* the authority has not noticed yet */
        assert(nrf_cs_authority_finish_external(&authority) == NRF_CS_AUTHORITY_COMPLETE);
        assert(!nrf_cs_authority_is_active(&authority));
        assert(nrf_cs_authority_finish_external(&authority) == NRF_CS_AUTHORITY_ERR_STATE); /* only once */
        assert(nrf_cs_authority_pair_at(&authority, 0U)->state == NRF_CS_PAIR_VALID);

        /* A command in flight means the authority is driving the cycle itself. */
        assert(nrf_cs_authority_start_cycle(&authority, three_nodes, 3U) == NRF_CS_AUTHORITY_PROGRESS);
        assert(nrf_cs_authority_pump(&authority) == NRF_CS_AUTHORITY_PROGRESS);
        assert(authority.command_active);
        assert(nrf_cs_authority_finish_external(&authority) == NRF_CS_AUTHORITY_ERR_STATE);
    }

    return 0;
}
