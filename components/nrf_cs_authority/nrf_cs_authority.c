#include "nrf_cs_authority.h"

#include <string.h>

#define NRF_CS_AUTHORITY_EXECUTE_RETRIES 2U
#define NRF_CS_AUTHORITY_POLL_PENDING 1

static uint32_t next_nonzero(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}

static bool request_uses_node(const nrf_cs_control_request_t *request, nrf_cs_node_id_t node)
{
    return request->step.initiator == node || request->step.reflector == node;
}

static void clear_command(nrf_cs_authority_t *authority)
{
    authority->command_active = false;
    authority->execute_acked = false;
    authority->execute_failures = 0U;
    authority->deadline_ms = 0U;
    memset(&authority->request, 0, sizeof(authority->request));
}

static nrf_cs_authority_result_t complete_measurement(
    nrf_cs_authority_t *authority,
    const nrf_cs_measurement_t *measurement)
{
    const nrf_cs_result_t result = nrf_cs_scheduler_complete_step(
        &authority->scheduler,
        &authority->request.step,
        measurement);
    if (result != NRF_CS_OK) {
        return result == NRF_CS_ERR_STALE ? NRF_CS_AUTHORITY_ERR_STALE : NRF_CS_AUTHORITY_ERR_STATE;
    }
    clear_command(authority);
    if (nrf_cs_scheduler_is_complete(&authority->scheduler)) {
        authority->cycle_active = false;
        return NRF_CS_AUTHORITY_COMPLETE;
    }
    return NRF_CS_AUTHORITY_PROGRESS;
}

nrf_cs_authority_result_t nrf_cs_authority_init(
    nrf_cs_authority_t *authority,
    const nrf_cs_policy_t *policy,
    const nrf_cs_authority_ops_t *ops,
    uint32_t request_seed)
{
    if (authority == NULL || policy == NULL || ops == NULL ||
        ops->write_execute == NULL || ops->poll_result == NULL || ops->now_ms == NULL) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    memset(authority, 0, sizeof(*authority));
    authority->ops = *ops;
    authority->next_request_id = request_seed;
    return nrf_cs_scheduler_init(&authority->scheduler, policy) == NRF_CS_OK
        ? NRF_CS_AUTHORITY_OK
        : NRF_CS_AUTHORITY_ERR_ARGUMENT;
}

nrf_cs_authority_result_t nrf_cs_authority_start_cycle(
    nrf_cs_authority_t *authority,
    const nrf_cs_node_id_t *nodes,
    size_t node_count)
{
    nrf_cs_result_t result;
    if (authority == NULL || (node_count > 0U && nodes == NULL)) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    if (authority->cycle_active || authority->command_active) {
        return NRF_CS_AUTHORITY_ERR_BUSY;
    }
    result = nrf_cs_scheduler_start_cycle(&authority->scheduler, nodes, node_count);
    if (result != NRF_CS_OK) {
        return result == NRF_CS_ERR_BUSY ? NRF_CS_AUTHORITY_ERR_BUSY : NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    authority->cycle_active = !nrf_cs_scheduler_is_complete(&authority->scheduler);
    return authority->cycle_active ? NRF_CS_AUTHORITY_PROGRESS : NRF_CS_AUTHORITY_COMPLETE;
}

nrf_cs_authority_result_t nrf_cs_authority_pump(nrf_cs_authority_t *authority)
{
    nrf_cs_control_result_t remote;
    nrf_cs_measurement_t failure = {.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR};
    uint64_t now;
    int io_result;

    if (authority == NULL) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    if (!authority->cycle_active) {
        return nrf_cs_scheduler_is_complete(&authority->scheduler)
            ? NRF_CS_AUTHORITY_COMPLETE
            : NRF_CS_AUTHORITY_ERR_STATE;
    }

    if (!authority->command_active) {
        nrf_cs_step_t step;
        const nrf_cs_result_t next = nrf_cs_scheduler_next_step(&authority->scheduler, &step);
        if (next == NRF_CS_CYCLE_COMPLETE) {
            authority->cycle_active = false;
            return NRF_CS_AUTHORITY_COMPLETE;
        }
        if (next != NRF_CS_STEP_READY) {
            return next == NRF_CS_ERR_BUSY ? NRF_CS_AUTHORITY_ERR_BUSY : NRF_CS_AUTHORITY_ERR_STATE;
        }
        authority->next_request_id = next_nonzero(authority->next_request_id);
        authority->request.request_id = authority->next_request_id;
        authority->request.step = step;
        authority->command_active = true;
        now = authority->ops.now_ms(authority->ops.context);
        authority->deadline_ms = now + (uint64_t)step.timeout_ms;
    }

    now = authority->ops.now_ms(authority->ops.context);
    if (now >= authority->deadline_ms) {
        failure.outcome = NRF_CS_OUTCOME_TIMEOUT;
        return complete_measurement(authority, &failure);
    }

    if (!authority->execute_acked) {
        io_result = authority->ops.write_execute(authority->ops.context, &authority->request);
        if (io_result == 0) {
            authority->execute_acked = true;
            return NRF_CS_AUTHORITY_PROGRESS;
        }
        ++authority->execute_failures;
        if (authority->execute_failures <= NRF_CS_AUTHORITY_EXECUTE_RETRIES) {
            return NRF_CS_AUTHORITY_PROGRESS;
        }
        return complete_measurement(authority, &failure);
    }

    memset(&remote, 0, sizeof(remote));
    io_result = authority->ops.poll_result(authority->ops.context, &authority->request, &remote);
    if (io_result == NRF_CS_AUTHORITY_POLL_PENDING) {
        return NRF_CS_AUTHORITY_PROGRESS;
    }
    if (io_result != 0) {
        /* A failed status read does not invalidate the remote radio attempt. */
        return NRF_CS_AUTHORITY_PROGRESS;
    }
    if (remote.request_id != authority->request.request_id ||
        !nrf_cs_control_step_equal(&remote.step, &authority->request.step)) {
        return NRF_CS_AUTHORITY_ERR_STALE;
    }
    if (remote.state == NRF_CS_CONTROL_STATE_RUNNING ||
        remote.command_result == NRF_CS_CONTROL_RESULT_PENDING) {
        return NRF_CS_AUTHORITY_PROGRESS;
    }
    if (remote.command_result == NRF_CS_CONTROL_RESULT_BUSY) {
        authority->execute_acked = false;
        authority->execute_failures = 0U;
        return NRF_CS_AUTHORITY_PROGRESS;
    }
    if (remote.command_result != NRF_CS_CONTROL_RESULT_OK) {
        return complete_measurement(authority, &failure);
    }
    return complete_measurement(authority, &remote.measurement);
}

nrf_cs_authority_result_t nrf_cs_authority_abort_cycle(
    nrf_cs_authority_t *authority,
    nrf_cs_failure_reason_t reason)
{
    if (authority == NULL || reason < NRF_CS_FAILURE_TIMEOUT ||
        reason > NRF_CS_FAILURE_DISCONNECTED) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    if (!authority->cycle_active) {
        return nrf_cs_scheduler_is_complete(&authority->scheduler)
            ? NRF_CS_AUTHORITY_COMPLETE
            : NRF_CS_AUTHORITY_ERR_STATE;
    }
    if (nrf_cs_scheduler_abort_cycle(&authority->scheduler, reason) != NRF_CS_OK) {
        return NRF_CS_AUTHORITY_ERR_STATE;
    }
    clear_command(authority);
    authority->cycle_active = false;
    return NRF_CS_AUTHORITY_COMPLETE;
}

nrf_cs_authority_result_t nrf_cs_authority_finish_external(nrf_cs_authority_t *authority)
{
    if (authority == NULL) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    if (!authority->cycle_active || !nrf_cs_scheduler_is_complete(&authority->scheduler)) {
        return NRF_CS_AUTHORITY_ERR_STATE;
    }
    authority->cycle_active = false;
    return NRF_CS_AUTHORITY_COMPLETE;
}

nrf_cs_authority_result_t nrf_cs_authority_disconnect(
    nrf_cs_authority_t *authority,
    nrf_cs_node_id_t node_id)
{
    nrf_cs_result_t result;
    if (authority == NULL) {
        return NRF_CS_AUTHORITY_ERR_ARGUMENT;
    }
    result = nrf_cs_scheduler_disconnect(&authority->scheduler, node_id);
    if (result != NRF_CS_OK) {
        return result == NRF_CS_ERR_NOT_FOUND ? NRF_CS_AUTHORITY_ERR_ARGUMENT : NRF_CS_AUTHORITY_ERR_STATE;
    }
    if (authority->command_active && request_uses_node(&authority->request, node_id)) {
        clear_command(authority);
    }
    if (nrf_cs_scheduler_is_complete(&authority->scheduler)) {
        authority->cycle_active = false;
        return NRF_CS_AUTHORITY_COMPLETE;
    }
    return NRF_CS_AUTHORITY_PROGRESS;
}

bool nrf_cs_authority_is_active(const nrf_cs_authority_t *authority)
{
    return authority != NULL && authority->cycle_active;
}

size_t nrf_cs_authority_pair_count(const nrf_cs_authority_t *authority)
{
    return authority == NULL ? 0U : nrf_cs_scheduler_pair_count(&authority->scheduler);
}

const nrf_cs_pair_result_t *nrf_cs_authority_pair_at(
    const nrf_cs_authority_t *authority,
    size_t pair_index)
{
    return authority == NULL ? NULL : nrf_cs_scheduler_pair_at(&authority->scheduler, pair_index);
}
